#include "openmoq/publisher/transport/libmoq_publisher.h"

#ifdef OPENMOQ_HAS_LIBMOQ

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <istream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <moq/rcbuf.h>
#include <moq/session.h>
#include <moq/types.h>

namespace openmoq::publisher::transport {

namespace {

moq_bytes_t bytes_of(const std::string& s) {
    return moq_bytes_t{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

moq_bytes_t bytes_of(const std::vector<std::uint8_t>& v) {
    return moq_bytes_t{v.data(), v.size()};
}

// Real media tracks carry an MP4 "vide"/"soun" handler. moqxr's synthetic
// catalog ("meta"/packaging "catalog") and generated timeline tracks
// (packaging "mediatimeline"/"eventtimeline") use the "meta" handler and must
// NOT be configured as libmoq media tracks -- libmoq owns catalog publication
// and derives any timeline tracks itself.
bool is_real_media_track(const TrackDescription& td) {
    return td.handler_type == "vide" || td.handler_type == "soun";
}

moq_media_type_t media_type_from_track(const TrackDescription& td) {
    if (td.handler_type == "soun") {
        return MOQ_MEDIA_TYPE_AUDIO;
    }
    if (td.handler_type == "vide") {
        return MOQ_MEDIA_TYPE_VIDEO;
    }
    // Fall back on shape: audio tracks carry a sample rate and no geometry.
    if (td.sample_rate != 0 && td.width == 0 && td.height == 0) {
        return MOQ_MEDIA_TYPE_AUDIO;
    }
    return MOQ_MEDIA_TYPE_VIDEO;
}

// Shared track config builder for the batch and live paths. moqxr's
// TrackDescription carries no bitrate, but MSF-01 5.2.22 requires a non-zero
// maxBitrate per audio/video track, so a bitrate_hint (0 = unknown) is supplied
// and a media-type default fills in when it is unavailable.
LibmoqTrackTranslation make_track_translation(const TrackDescription& td,
                                              std::vector<std::uint8_t> init_data,
                                              std::uint64_t bitrate_hint,
                                              bool is_live) {
    LibmoqTrackTranslation t;
    t.name = !td.track_name.empty() ? td.track_name
                                    : ("track" + std::to_string(td.track_id));
    t.media_type = media_type_from_track(td);
    t.packaging = MOQ_MEDIA_PACKAGING_CMAF;
    t.codec = td.codec;
    t.timescale = 0;  // object times are already microseconds
    t.is_live = is_live;
    t.width = td.width;
    t.height = td.height;
    t.framerate_millis = static_cast<std::uint64_t>(td.frame_rate * 1000.0 + 0.5);
    if (t.media_type == MOQ_MEDIA_TYPE_AUDIO) {
        t.samplerate = td.sample_rate != 0 ? td.sample_rate : 48000;
        t.channel_config =
            td.channel_count != 0 ? std::to_string(td.channel_count) : std::string("2");
    }
    t.bitrate = bitrate_hint;
    if (t.bitrate == 0) {
        t.bitrate = t.media_type == MOQ_MEDIA_TYPE_AUDIO ? 128000ull : 2000000ull;
    }
    t.init_data = std::move(init_data);
    return t;
}

}  // namespace

moq_media_track_cfg_t LibmoqTrackTranslation::cfg() const {
    moq_media_track_cfg_t c;
    moq_media_track_cfg_init(&c);
    c.name = bytes_of(name);
    c.media_type = media_type;
    c.packaging = packaging;
    c.codec = bytes_of(codec);
    c.timescale = timescale;
    if (!init_data.empty()) {
        c.init_data = bytes_of(init_data);
    }
    c.is_live = is_live;
    c.width = width;
    c.height = height;
    c.framerate_millis = framerate_millis;
    c.samplerate = samplerate;
    if (!channel_config.empty()) {
        c.channel_config = bytes_of(channel_config);
    }
    c.bitrate = bitrate;
    return c;
}

moq_media_send_object_t LibmoqObjectTranslation::object() const {
    moq_media_send_object_t o;
    std::memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.payload = nullptr;     // caller fills with a freshly created rcbuf
    o.properties = nullptr;  // CMAF: timing rides in the fragment; no extra block
    o.is_sync = is_sync;
    o.starts_group = starts_group;
    o.ends_group = ends_group;
    o.has_sap_type = has_sap_type;  // declared SAP lets a coalesced/live group-start
    o.sap_type = sap_type;          // pass libmoq's §3.4 group-start rule
    o.decode_time_us = decode_time_us;
    o.presentation_time_us = presentation_time_us;
    return o;
}

namespace {
// Map moqxr's uint8 SAP type (0..3) onto the libmoq enum, declaring only the
// concrete values moqxr actually computed -- never fabricating one. A value
// outside 0..3 (e.g. an unknown/unset sentinel) leaves has_sap_type=false, so
// libmoq falls back to its sync-sample check. A declared 0 (NONE) at a group
// start is left as-is and will be (correctly) rejected by libmoq's §3.4 rule.
void apply_sap(LibmoqObjectTranslation& o, bool src_has, std::uint8_t value) {
    o.has_sap_type = false;
    o.sap_type = MOQ_SAP_NONE;
    if (!src_has) {
        return;
    }
    switch (value) {
        case 0: o.sap_type = MOQ_SAP_NONE;   o.has_sap_type = true; break;
        case 1: o.sap_type = MOQ_SAP_TYPE_1; o.has_sap_type = true; break;
        case 2: o.sap_type = MOQ_SAP_TYPE_2; o.has_sap_type = true; break;
        case 3: o.sap_type = MOQ_SAP_TYPE_3; o.has_sap_type = true; break;
        default: break;  // unknown/out-of-range: do not declare
    }
}
}  // namespace

LibmoqPlanTranslation translate_plan_for_libmoq(const PublishPlan& plan) {
    LibmoqPlanTranslation out;

    // moqxr's TrackDescription carries no bitrate, but MSF-01 5.2.22 requires a
    // non-zero maxBitrate per audio/video track. Derive an estimate from the
    // observed media payload (bytes*8 / duration) and fall back when unknown.
    struct Accum {
        std::uint64_t bytes = 0;
        std::uint64_t duration_us = 0;
    };
    std::unordered_map<std::string, Accum> accum;

    // Highest media object_id per (track, group), for ends_group.
    std::map<std::pair<std::string, std::size_t>, std::size_t> last_object_in_group;

    for (const auto& obj : plan.objects) {
        if (obj.kind != CmsfObjectKind::kMedia) {
            continue;
        }
        const auto key = std::make_pair(obj.track_name, obj.group_id);
        auto it = last_object_in_group.find(key);
        if (it == last_object_in_group.end() || obj.object_id > it->second) {
            last_object_in_group[key] = obj.object_id;
        }
        const std::size_t bytes =
            !obj.owned_payload.empty() ? obj.owned_payload.size() : obj.payload.size;
        Accum& a = accum[obj.track_name];
        a.bytes += static_cast<std::uint64_t>(bytes);
        a.duration_us += obj.media_duration_us;
    }

    out.tracks.reserve(plan.tracks.size());
    for (const auto& td : plan.tracks) {
        if (!is_real_media_track(td)) {
            continue;  // skip catalog + mediatimeline/eventtimeline synthetic tracks
        }
        const std::string name =
            !td.track_name.empty() ? td.track_name : ("track" + std::to_string(td.track_id));
        std::vector<std::uint8_t> init_data;
        for (const auto& init : plan.track_initializations) {
            if (init.track_name == name) {
                init_data = init.init_segment;
                break;
            }
        }
        std::uint64_t bitrate = 0;
        const auto a = accum.find(name);
        if (a != accum.end() && a->second.duration_us > 0) {
            bitrate = (a->second.bytes * 8ull * 1000000ull) / a->second.duration_us;
        }
        out.tracks.push_back(
            make_track_translation(td, std::move(init_data), bitrate, /*is_live=*/false));
    }

    for (const auto& obj : plan.objects) {
        if (obj.kind != CmsfObjectKind::kMedia) {
            continue;  // skip catalog (kInitialization) + timelines (kMetadata)
        }
        LibmoqObjectTranslation o;
        o.track_name = obj.track_name;
        o.group_id = obj.group_id;
        o.object_id = obj.object_id;
        o.starts_group = obj.object_id == 0;
        const auto it = last_object_in_group.find({obj.track_name, obj.group_id});
        o.ends_group = it != last_object_in_group.end() && obj.object_id == it->second;
        o.is_sync = o.starts_group;  // group starts are SAPs; no finer metadata yet
        apply_sap(o, obj.has_sap_type, obj.sap_type);  // carry SAP into the send object
        o.decode_time_us = obj.media_time_us;
        o.presentation_time_us = obj.media_time_us;
        if (!obj.owned_payload.empty()) {
            o.payload = obj.owned_payload;
        }
        out.objects.push_back(std::move(o));
    }

    return out;
}

std::string libmoq_endpoint_url(const EndpointConfig& endpoint) {
    const bool wt = endpoint.transport == TransportKind::kWebTransport;
    std::string path = endpoint.path.empty() ? std::string("/") : endpoint.path;
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return (wt ? std::string("https://") : std::string("moqt://")) + endpoint.host + ":" +
           std::to_string(endpoint.port) + path;
}

LibmoqTrackTranslation make_libmoq_live_track(const TrackDescription& track,
                                              const std::vector<std::uint8_t>& init_segment) {
    // Live tracks are isLive; no upfront stats, so the bitrate falls back to a
    // media-type default inside make_track_translation (hint = 0).
    return make_track_translation(track, init_segment, /*bitrate_hint=*/0, /*is_live=*/true);
}

LibmoqObjectTranslation make_libmoq_live_object(const MediaFragment& fragment) {
    LibmoqObjectTranslation o;
    o.track_name = fragment.track_name;
    o.group_id = fragment.group_id;
    o.object_id = fragment.object_id;
    o.starts_group = fragment.object_id == 0;
    o.ends_group = false;  // streaming: closed by the next group start / end_track
    // Live fragments carry richer sync metadata than the batch plan: a video
    // keyframe or a *declared* SAP type marks a random-access point, as does any
    // group start (group boundaries are driven by video keyframes here). Only a
    // declared SAP counts -- a stale/undeclared sap_type must not imply sync.
    o.is_sync = o.starts_group || fragment.is_video_keyframe ||
                (fragment.has_sap_type && fragment.sap_type != 0);
    apply_sap(o, fragment.has_sap_type, fragment.sap_type);  // carry SAP into the send object
    o.decode_time_us = fragment.start_time_us;
    o.presentation_time_us = fragment.earliest_presentation_time_us != 0
                                 ? fragment.earliest_presentation_time_us
                                 : fragment.start_time_us;
    o.payload = fragment.payload.owned_bytes;
    return o;
}

bool live_track_has_media_metadata(const LiveTrack& track) {
    if (track.track_name.empty()) {
        return false;
    }
    if (track.media_type == LiveMediaType::kUnset) {
        return false;  // bare legacy track: name + opaque payloads only
    }
    if (track.codec.empty()) {
        return false;  // MSF-01 5.2.18: codec required for audio/video
    }
    if (track.media_type == LiveMediaType::kAudio &&
        (track.sample_rate == 0 || track.channel_count == 0)) {
        return false;  // MSF-01 5.2.28/5.2.29: audio needs samplerate + channels
    }
    return true;
}

LibmoqTrackTranslation make_libmoq_live_object_track(const LiveTrack& track) {
    LibmoqTrackTranslation t;
    t.name = track.track_name;
    t.media_type =
        track.media_type == LiveMediaType::kAudio ? MOQ_MEDIA_TYPE_AUDIO : MOQ_MEDIA_TYPE_VIDEO;
    t.packaging =
        track.packaging == LivePackaging::kCmaf ? MOQ_MEDIA_PACKAGING_CMAF : MOQ_MEDIA_PACKAGING_RAW;
    t.codec = track.codec;
    t.timescale = 0;  // object times are microseconds
    t.init_data = track.init_data;
    t.is_live = true;
    t.width = track.width;
    t.height = track.height;
    t.framerate_millis = 0;  // not carried by LiveTrack
    if (t.media_type == MOQ_MEDIA_TYPE_AUDIO) {
        t.samplerate = track.sample_rate;
        t.channel_config =
            track.channel_count != 0 ? std::to_string(track.channel_count) : std::string("2");
    }
    t.bitrate = track.bitrate != 0
                    ? track.bitrate
                    : (t.media_type == MOQ_MEDIA_TYPE_AUDIO ? 128000ull : 2000000ull);
    return t;
}

LibmoqObjectTranslation make_libmoq_live_source_object(const LiveObject& object) {
    LibmoqObjectTranslation o;
    o.track_name = object.track_name;
    o.group_id = object.group_id;
    o.object_id = object.object_id;
    o.starts_group = object.object_id == 0;
    o.ends_group = object.final_in_subgroup && object.subgroup_contains_group_largest;
    o.is_sync = object.object_id == 0;  // group starts are SAPs for now
    o.decode_time_us = object.media_time_us;
    o.presentation_time_us = object.media_time_us;
    o.payload = object.payload;
    return o;
}

namespace {

bool cancelled(const std::atomic<bool>* cancel) {
    return cancel != nullptr && cancel->load();
}

// Records subscriber-demand events from the sender's network thread. The
// media-sender demand callbacks are non-reentrant and signal-only, so they just
// bump a generation counter + notify the CV; the app thread re-checks the
// authoritative moq_media_sender_has_media_subscriber() query after a wake.
struct DemandMonitor {
    std::mutex mutex;
    std::condition_variable cv;

    void notify() {
        {
            std::lock_guard<std::mutex> lock(mutex);
        }
        cv.notify_all();
    }
};

void on_demand_joined(void* ctx, moq_media_sender_t* /*sender*/,
                      moq_media_track_t* /*track*/, size_t /*active*/) {
    if (ctx != nullptr) {
        static_cast<DemandMonitor*>(ctx)->notify();
    }
}

void on_demand_left(void* ctx, moq_media_sender_t* /*sender*/,
                    moq_media_track_t* /*track*/, size_t /*active*/) {
    if (ctx != nullptr) {
        static_cast<DemandMonitor*>(ctx)->notify();
    }
}

TransportStatus teardown(moq_media_sender_t* sender,
                         moq_endpoint_t* ep,
                         const std::string& error) {
    if (sender) {
        moq_media_sender_destroy(sender);
    }
    if (ep) {
        moq_endpoint_stop(ep);
        moq_endpoint_destroy(ep);
    }
    return error.empty() ? TransportStatus::success() : TransportStatus::failure(error);
}

// Tear down a live publish, first unregistering the endpoint from the shared
// handle (under its mutex) so a concurrent disconnect() cannot touch the
// endpoint while it is being destroyed.
TransportStatus live_teardown(LibmoqLiveHandle* live, moq_media_sender_t* sender,
                              moq_endpoint_t* ep, const std::string& error) {
    if (live != nullptr) {
        live->set_endpoint(nullptr);
    }
    return teardown(sender, ep, error);
}

// Clean stop on a disconnect()-initiated cancel: publish the stats gathered so
// far and tear down without the strict end_track/drain error checks. Returns
// success -- cancellation is a deliberate stop, matching the legacy MoqtSession
// close(). The endpoint interrupt latch was already set by request_cancel().
TransportStatus cancel_teardown(LibmoqLiveHandle* live, moq_media_sender_t* sender,
                                moq_endpoint_t* ep, const LibmoqPublishStats& stats,
                                LibmoqPublishStats& out_stats) {
    out_stats = stats;
    return live_teardown(live, sender, ep, "");
}

bool draft_to_version(DraftVersion draft, moq_version_t* out) {
    switch (draft) {
        case DraftVersion::kDraft16:
            *out = MOQ_VERSION_DRAFT_16;
            return true;
        case DraftVersion::kDraft18:
            *out = MOQ_VERSION_DRAFT_18;
            return true;
        default:
            return false;
    }
}

// Connect a managed endpoint and attach a media sender for the configured
// namespace, requesting the configured draft exactly. `live` selects the
// backpressure preset (live: drop-to-keyframe; batch: lossless block). On
// success *out_ep and *out_sender are set and the caller owns teardown; on
// failure everything is torn down and a failure status returned. The backing
// strings live for the whole call, which spans connect() and attach() -- both
// copy what they retain (the handshake/verifier run after connect returns).
//
// This does NOT wait for readiness: callers must add their tracks first, then
// wait_ready(), so the initial retained catalog is built from the configured
// tracks rather than published empty and then republished post-add.
TransportStatus connect_and_attach(const PublisherConfig& config,
                                   const EndpointConfig& endpoint,
                                   const TlsConfig& tls,
                                   bool live,
                                   DemandMonitor* demand,
                                   moq_endpoint_t** out_ep,
                                   moq_media_sender_t** out_sender) {
    *out_ep = nullptr;
    *out_sender = nullptr;

    moq_version_t version;
    if (!draft_to_version(config.draft_version, &version)) {
        return TransportStatus::failure(
            "libmoq publish supports only draft 16 and draft 18");
    }

    const std::string url = libmoq_endpoint_url(endpoint);
    const std::string sni = endpoint.sni;
    const std::string ca = tls.ca_path;
    const std::string wt_path = endpoint.path;
    const std::string ns_str = config.track_namespace;

    moq_endpoint_cfg_t ep_cfg;
    moq_endpoint_cfg_init(&ep_cfg);
    ep_cfg.url = moq_bytes_t{reinterpret_cast<const std::uint8_t*>(url.data()), url.size()};
    ep_cfg.protocol = endpoint.transport == TransportKind::kWebTransport
                          ? MOQ_TRANSPORT_PROTOCOL_WEBTRANSPORT
                          : MOQ_TRANSPORT_PROTOCOL_RAW_QUIC;
    moq_version_t versions[1] = {version};
    ep_cfg.versions.struct_size = sizeof(ep_cfg.versions);
    ep_cfg.versions.policy = MOQ_VERSION_POLICY_EXACT;  // request the configured draft exactly
    ep_cfg.versions.versions = versions;
    ep_cfg.versions.version_count = 1;
    if (!sni.empty()) {
        ep_cfg.sni = moq_bytes_t{reinterpret_cast<const std::uint8_t*>(sni.data()), sni.size()};
    }
    if (!ca.empty()) {
        ep_cfg.ca_file = moq_bytes_t{reinterpret_cast<const std::uint8_t*>(ca.data()), ca.size()};
    }
    ep_cfg.insecure_skip_verify = tls.insecure_skip_verify;
    if (endpoint.transport == TransportKind::kWebTransport && !wt_path.empty()) {
        ep_cfg.wt_path =
            moq_bytes_t{reinterpret_cast<const std::uint8_t*>(wt_path.data()), wt_path.size()};
    }

    moq_endpoint_t* ep = nullptr;
    moq_result_t rc = moq_endpoint_connect(&ep_cfg, &ep);
    if (rc != MOQ_OK || ep == nullptr) {
        return TransportStatus::failure(std::string("endpoint connect failed: ") +
                                        moq_strerror(rc));
    }

    moq_bytes_t ns_part{reinterpret_cast<const std::uint8_t*>(ns_str.data()), ns_str.size()};
    moq_namespace_t ns{&ns_part, 1};

    moq_media_sender_cfg_t s_cfg;
    if (live) {
        moq_media_sender_cfg_init_live(&s_cfg);  // never block the encoder
    } else {
        moq_media_sender_cfg_init_lossless(&s_cfg);  // batch/VOD: never drop on our own
    }
    s_cfg.endpoint = nullptr;  // attach mode borrows the endpoint
    s_cfg.namespace_ = ns;
    if (demand != nullptr) {
        // Demand-visibility callbacks (network-thread, signal-only) so the app
        // thread can wait for a real subscriber instead of blindly streaming.
        moq_media_sender_callbacks_init(&s_cfg.callbacks);
        s_cfg.callbacks.ctx = demand;
        s_cfg.callbacks.on_subscriber_joined = on_demand_joined;
        s_cfg.callbacks.on_subscriber_left = on_demand_left;
    }

    moq_media_sender_t* sender = nullptr;
    rc = moq_media_sender_attach(ep, &s_cfg, &sender);
    if (rc != MOQ_OK || sender == nullptr) {
        return teardown(nullptr, ep,
                        std::string("media sender attach failed: ") + moq_strerror(rc));
    }

    *out_ep = ep;
    *out_sender = sender;
    return TransportStatus::success();
}

// Wait for namespace acceptance + catalog publication. Call AFTER tracks are
// added so the initial catalog carries them. Builds the real libmoq ops and
// delegates to the unit-testable libmoq_wait_ready() primitive. A cancel during
// the wait returns kCancelled promptly: request_cancel() set the endpoint
// interrupt latch, so the underlying moq_endpoint_wait() returns at once.
LibmoqReadyOutcome wait_ready(moq_endpoint_t* ep, moq_media_sender_t* sender,
                              std::atomic<bool>* cancel, std::uint64_t timeout_us) {
    LibmoqReadyOps ops;
    ops.is_ready = [sender] { return moq_media_sender_is_ready(sender); };
    ops.is_fatal = [sender, ep] {
        return moq_media_sender_is_fatal(sender) || moq_endpoint_is_fatal(ep);
    };
    ops.wait = [ep](std::uint64_t step) { return static_cast<int>(moq_endpoint_wait(ep, step)); };
    return libmoq_wait_ready(cancel, timeout_us, /*step_us=*/100000, ops);
}

// Map a non-ready readiness outcome to a teardown. Precondition: outcome is not
// kReady (and the caller handles kCancelled itself, which needs the stats).
TransportStatus ready_failure_teardown(LibmoqLiveHandle* live, moq_media_sender_t* sender,
                                       moq_endpoint_t* ep, LibmoqReadyOutcome outcome) {
    switch (outcome) {
        case LibmoqReadyOutcome::kFatal:
            return live_teardown(live, sender, ep, "endpoint/sender became fatal before readiness");
        case LibmoqReadyOutcome::kTimeout:
            return live_teardown(live, sender, ep, "timed out waiting for media sender readiness");
        case LibmoqReadyOutcome::kClosed:
            return live_teardown(live, sender, ep, "endpoint closed before readiness");
        case LibmoqReadyOutcome::kReady:
        case LibmoqReadyOutcome::kCancelled:
            break;
    }
    return live_teardown(live, sender, ep, "media sender readiness failed");
}

// Wait for at least one downstream media subscriber. Builds the real libmoq ops
// (the demand callback nudges the CV; has_media_subscriber() is authoritative)
// and delegates to the unit-testable libmoq_wait_demand() primitive. Bounded by
// timeout_us (PublisherConfig::subscriber_timeout) so it never hangs.
LibmoqDemandOutcome wait_for_media_subscriber(moq_endpoint_t* ep, moq_media_sender_t* sender,
                                              DemandMonitor* demand, std::atomic<bool>* cancel,
                                              std::uint64_t timeout_us) {
    LibmoqDemandOps ops;
    ops.has_subscriber = [sender] { return moq_media_sender_has_media_subscriber(sender); };
    ops.is_fatal = [sender, ep] {
        return moq_media_sender_is_fatal(sender) || moq_endpoint_is_fatal(ep);
    };
    ops.is_closed = [sender, ep] {
        return moq_media_sender_is_closed(sender) || moq_endpoint_is_closed(ep);
    };
    ops.wait = [demand](std::uint64_t step) {
        std::unique_lock<std::mutex> lock(demand->mutex);
        demand->cv.wait_for(lock, std::chrono::microseconds(step));
    };
    return libmoq_wait_demand(cancel, timeout_us, /*step_us=*/100000, ops);
}

// Map a non-subscriber demand outcome to a teardown (caller handles kSubscriber
// and kCancelled, which needs the stats).
TransportStatus demand_failure_teardown(LibmoqLiveHandle* live, moq_media_sender_t* sender,
                                        moq_endpoint_t* ep, LibmoqDemandOutcome outcome) {
    switch (outcome) {
        case LibmoqDemandOutcome::kFatal:
            return live_teardown(live, sender, ep,
                                 "endpoint/sender became fatal before a media subscriber");
        case LibmoqDemandOutcome::kTimeout:
            return live_teardown(live, sender, ep, "timed out waiting for media subscriber");
        case LibmoqDemandOutcome::kClosed:
            return live_teardown(live, sender, ep, "endpoint closed before a media subscriber");
        case LibmoqDemandOutcome::kSubscriber:
        case LibmoqDemandOutcome::kCancelled:
            break;
    }
    return live_teardown(live, sender, ep, "media subscriber wait failed");
}

// Bounded retry of moq_media_sender_write(): builds real ops and delegates to
// libmoq_retry_blocking. check_demand=true (batch media) bails kNoDemand if the
// subscriber leaves mid-write. Caller owns the payload rcbuf on any non-kOk.
LibmoqRetryOutcome retry_write(moq_media_sender_t* sender, moq_endpoint_t* ep,
                               moq_media_track_t* track, const moq_media_send_object_t* obj,
                               std::atomic<bool>* cancel, bool check_demand,
                               std::uint64_t timeout_us, int* out_rc) {
    LibmoqRetryOps ops;
    ops.attempt = [sender, track, obj] {
        return static_cast<int>(moq_media_sender_write(sender, track, obj));
    };
    ops.is_fatal = [sender, ep] {
        return moq_media_sender_is_fatal(sender) || moq_endpoint_is_fatal(ep);
    };
    ops.is_closed = [sender, ep] {
        return moq_media_sender_is_closed(sender) || moq_endpoint_is_closed(ep);
    };
    if (check_demand) {
        ops.has_demand = [sender] { return moq_media_sender_has_media_subscriber(sender); };
    }
    ops.wait = [ep](std::uint64_t step) { moq_endpoint_wait(ep, step); };
    return libmoq_retry_blocking(cancel, timeout_us, /*step_us=*/50000, ops, out_rc);
}

// Bounded retry of moq_media_sender_end_track(). No demand check: end_track
// completes locally even with no subscriber, so a WOULD_BLOCK here means the
// send queue is momentarily full -- bound it on cancel/fatal/closed/timeout.
LibmoqRetryOutcome retry_end_track(moq_media_sender_t* sender, moq_endpoint_t* ep,
                                   moq_media_track_t* track, std::atomic<bool>* cancel,
                                   std::uint64_t timeout_us, int* out_rc) {
    LibmoqRetryOps ops;
    ops.attempt = [sender, track] {
        return static_cast<int>(moq_media_sender_end_track(sender, track));
    };
    ops.is_fatal = [sender, ep] {
        return moq_media_sender_is_fatal(sender) || moq_endpoint_is_fatal(ep);
    };
    ops.is_closed = [sender, ep] {
        return moq_media_sender_is_closed(sender) || moq_endpoint_is_closed(ep);
    };
    ops.wait = [ep](std::uint64_t step) { moq_endpoint_wait(ep, step); };
    return libmoq_retry_blocking(cancel, timeout_us, /*step_us=*/50000, ops, out_rc);
}

// Map a non-OK (and non-cancelled) retry outcome to a teardown with a clear
// reason. `what` names the op (e.g. "media write", "end_track").
TransportStatus retry_failure_teardown(LibmoqLiveHandle* live, moq_media_sender_t* sender,
                                       moq_endpoint_t* ep, LibmoqRetryOutcome outcome, int rc,
                                       const char* what) {
    switch (outcome) {
        case LibmoqRetryOutcome::kFatal:
            return live_teardown(live, sender, ep,
                                 std::string("endpoint/sender became fatal during ") + what);
        case LibmoqRetryOutcome::kClosed:
            return live_teardown(live, sender, ep, std::string("endpoint closed during ") + what);
        case LibmoqRetryOutcome::kTimeout:
            return live_teardown(live, sender, ep,
                                 std::string("timed out waiting for media send queue capacity (") +
                                     what + ")");
        case LibmoqRetryOutcome::kNoDemand:
            return live_teardown(live, sender, ep, "media subscriber disappeared during publish");
        case LibmoqRetryOutcome::kError:
            return live_teardown(live, sender, ep,
                                 std::string(what) + " failed: " + moq_strerror(rc));
        case LibmoqRetryOutcome::kOk:
        case LibmoqRetryOutcome::kCancelled:
            break;
    }
    return live_teardown(live, sender, ep, std::string(what) + " failed");
}

// Wait for the sender's queue to drain to the wire (bounded by timeout_us; 0 =
// wait indefinitely until drained or fatal). Returns early on cancel.
void drain_sender(moq_endpoint_t* ep, moq_media_sender_t* sender, std::uint64_t timeout_us,
                  const std::atomic<bool>* cancel = nullptr) {
    std::uint64_t waited = 0;
    for (;;) {
        moq_media_sender_stats_t ms;
        if (moq_media_sender_get_stats(sender, &ms, sizeof(ms)) == MOQ_OK &&
            ms.objects_queued == 0 && ms.objects_sent >= ms.objects_written) {
            break;
        }
        if (cancelled(cancel)) {
            break;
        }
        if (moq_media_sender_is_fatal(sender) || moq_endpoint_is_fatal(ep)) {
            break;
        }
        if (timeout_us != 0 && waited >= timeout_us) {
            break;
        }
        moq_endpoint_wait(ep, 100000);
        waited += 100000;
    }
}

// Write one translated live object: create the payload rcbuf, hand it to the
// sender (ownership transfers on MOQ_OK), and on success bump the stats. On a
// non-OK return the caller's ref is released here and the result code is
// returned -- under the live drop policy WOULD_BLOCK is an expected non-fatal
// drop, so the caller only fails on sender/endpoint fatal states.
moq_result_t write_live_object(moq_media_sender_t* sender, moq_media_track_t* track,
                               const moq_alloc_t* alloc,
                               const LibmoqObjectTranslation& translated,
                               LibmoqPublishStats& stats,
                               std::set<std::pair<std::string, std::size_t>>& groups) {
    moq_rcbuf_t* buf = nullptr;
    moq_result_t rc =
        moq_rcbuf_create(alloc, translated.payload.data(), translated.payload.size(), &buf);
    if (rc != MOQ_OK || buf == nullptr) {
        return rc != MOQ_OK ? rc : MOQ_ERR_NOMEM;
    }
    moq_media_send_object_t so = translated.object();
    so.payload = buf;
    rc = moq_media_sender_write(sender, track, &so);
    if (rc == MOQ_OK) {
        stats.bytes_published += static_cast<std::uint64_t>(translated.payload.size());
        stats.objects_published += 1;
        groups.emplace(translated.track_name, translated.group_id);
        return MOQ_OK;
    }
    moq_rcbuf_decref(buf);  // no ownership transfer on a non-OK write
    return rc;
}

}  // namespace

LibmoqReadyOutcome libmoq_wait_ready(std::atomic<bool>* cancel,
                                     std::uint64_t timeout_us, std::uint64_t step_us,
                                     const LibmoqReadyOps& ops) {
    std::uint64_t waited = 0;
    while (!ops.is_ready()) {
        if (cancel != nullptr && cancel->load()) {
            return LibmoqReadyOutcome::kCancelled;
        }
        if (ops.is_fatal()) {
            return LibmoqReadyOutcome::kFatal;
        }
        if (timeout_us != 0 && waited >= timeout_us) {
            return LibmoqReadyOutcome::kTimeout;
        }
        if (ops.wait(step_us) == MOQ_ERR_CLOSED) {
            return LibmoqReadyOutcome::kClosed;
        }
        waited += step_us;
    }
    return LibmoqReadyOutcome::kReady;
}

LibmoqDemandOutcome libmoq_wait_demand(std::atomic<bool>* cancel,
                                       std::uint64_t timeout_us, std::uint64_t step_us,
                                       const LibmoqDemandOps& ops) {
    std::uint64_t waited = 0;
    for (;;) {
        if (ops.has_subscriber()) {  // authoritative: return as soon as demand exists
            return LibmoqDemandOutcome::kSubscriber;
        }
        if (cancel != nullptr && cancel->load()) {
            return LibmoqDemandOutcome::kCancelled;
        }
        if (ops.is_fatal()) {
            return LibmoqDemandOutcome::kFatal;
        }
        if (ops.is_closed()) {
            return LibmoqDemandOutcome::kClosed;
        }
        if (timeout_us != 0 && waited >= timeout_us) {
            return LibmoqDemandOutcome::kTimeout;
        }
        ops.wait(step_us);  // woken by the demand callback or after step_us
        waited += step_us;
    }
}

LibmoqRetryOutcome libmoq_retry_blocking(std::atomic<bool>* cancel,
                                         std::uint64_t timeout_us, std::uint64_t step_us,
                                         const LibmoqRetryOps& ops, int* out_rc) {
    std::uint64_t waited = 0;
    for (;;) {
        const int rc = ops.attempt();
        if (out_rc != nullptr) {
            *out_rc = rc;
        }
        if (rc == MOQ_OK) {
            return LibmoqRetryOutcome::kOk;
        }
        if (rc != MOQ_ERR_WOULD_BLOCK) {
            return LibmoqRetryOutcome::kError;  // a genuine failure, not backpressure
        }
        // WOULD_BLOCK: decide whether to keep retrying or bail with a clear reason.
        if (cancel != nullptr && cancel->load()) {
            return LibmoqRetryOutcome::kCancelled;
        }
        if (ops.is_fatal()) {
            return LibmoqRetryOutcome::kFatal;
        }
        if (ops.is_closed()) {
            return LibmoqRetryOutcome::kClosed;
        }
        if (ops.has_demand && !ops.has_demand()) {
            return LibmoqRetryOutcome::kNoDemand;
        }
        if (timeout_us != 0 && waited >= timeout_us) {
            return LibmoqRetryOutcome::kTimeout;
        }
        ops.wait(step_us);
        waited += step_us;
    }
}

TransportStatus publish_plan_via_libmoq(const PublishPlan& materialized_plan,
                                        const PublisherConfig& config,
                                        const EndpointConfig& endpoint,
                                        const TlsConfig& tls,
                                        LibmoqPublishStats& out_stats) {
    DemandMonitor demand;
    moq_endpoint_t* ep = nullptr;
    moq_media_sender_t* sender = nullptr;
    const TransportStatus setup =
        connect_and_attach(config, endpoint, tls, /*live=*/false, &demand, &ep, &sender);
    if (!setup.ok) {
        return setup;
    }

    const std::uint64_t timeout_us =
        static_cast<std::uint64_t>(config.subscriber_timeout.count()) * 1000000ull;
    moq_result_t rc = MOQ_OK;

    const LibmoqPlanTranslation tr = translate_plan_for_libmoq(materialized_plan);

    std::unordered_map<std::string, moq_media_track_t*> handles;
    for (const auto& t : tr.tracks) {
        moq_media_track_cfg_t c = t.cfg();
        moq_media_track_t* h = nullptr;
        rc = moq_media_sender_add_track(sender, &c, &h);
        if (rc != MOQ_OK || h == nullptr) {
            return teardown(sender, ep,
                            "add_track failed for '" + t.name + "': " + moq_strerror(rc));
        }
        handles[t.name] = h;
    }

    // Tracks are configured: now wait for the initial catalog to publish.
    const LibmoqReadyOutcome ready = wait_ready(ep, sender, /*cancel=*/nullptr, timeout_us);
    if (ready != LibmoqReadyOutcome::kReady) {
        return ready_failure_teardown(/*live=*/nullptr, sender, ep, ready);
    }

    // Wait for a real subscriber before writing: against a lazy relay the
    // lossless preset would otherwise fill the queue and block here forever.
    const LibmoqDemandOutcome demand_out =
        wait_for_media_subscriber(ep, sender, &demand, /*cancel=*/nullptr, timeout_us);
    if (demand_out != LibmoqDemandOutcome::kSubscriber) {
        return demand_failure_teardown(/*live=*/nullptr, sender, ep, demand_out);
    }

    const moq_alloc_t* alloc = moq_alloc_default();
    LibmoqPublishStats stats;
    std::set<std::pair<std::string, std::size_t>> groups;
    for (const auto& o : tr.objects) {
        const auto hit = handles.find(o.track_name);
        if (hit == handles.end()) {
            continue;  // media object for a track that was not configured
        }
        moq_rcbuf_t* buf = nullptr;
        rc = moq_rcbuf_create(alloc, o.payload.data(), o.payload.size(), &buf);
        if (rc != MOQ_OK || buf == nullptr) {
            return teardown(sender, ep, "payload buffer allocation failed");
        }
        moq_media_send_object_t so = o.object();
        so.payload = buf;
        // Bounded retry: if the subscriber leaves or the queue never drains, bail
        // with a clear reason instead of spinning forever on WOULD_BLOCK.
        int wrc = MOQ_OK;
        const LibmoqRetryOutcome wout = retry_write(sender, ep, hit->second, &so,
                                                    /*cancel=*/nullptr, /*check_demand=*/true,
                                                    timeout_us, &wrc);
        if (wout != LibmoqRetryOutcome::kOk) {
            moq_rcbuf_decref(buf);  // no ownership transfer on a non-OK write
            return retry_failure_teardown(/*live=*/nullptr, sender, ep, wout, wrc, "media write");
        }
        // kOk transfers the buf ref to the sender; do not decref.
        stats.bytes_published += static_cast<std::uint64_t>(o.payload.size());
        stats.objects_published += 1;
        groups.emplace(o.track_name, o.group_id);
    }
    stats.groups_published = static_cast<std::uint64_t>(groups.size());

    for (const auto& t : tr.tracks) {
        const auto hit = handles.find(t.name);
        if (hit == handles.end()) {
            continue;
        }
        int erc = MOQ_OK;
        const LibmoqRetryOutcome eout =
            retry_end_track(sender, ep, hit->second, /*cancel=*/nullptr, timeout_us, &erc);
        if (eout != LibmoqRetryOutcome::kOk) {
            return retry_failure_teardown(/*live=*/nullptr, sender, ep, eout, erc, "end_track");
        }
    }

    // Let the queued objects drain to the wire before tearing down.
    drain_sender(ep, sender, timeout_us);

    // drain_sender() only confirms the SENDER queue is empty (objects emitted to
    // the session) -- not that the transport flushed them. moq_endpoint_drain()
    // blocks (bounded by timeout_us) until libmoq/picoquic has flushed the local
    // stream bytes + FIN from its send queues, so teardown does not truncate a
    // slow/in-flight object (it does not wait for peer consumption or full ACK).
    // Best-effort before teardown: a timeout (MOQ_DONE), interrupt, or a backend
    // that cannot prove a flush (MOQ_ERR_UNSUPPORTED) all fall through to the stop
    // below -- the publish already succeeded once the objects were written.
    moq_endpoint_drain(ep, timeout_us);

    out_stats = stats;
    return teardown(sender, ep, "");
}

TransportStatus publish_live_stdin_via_libmoq(std::istream& input,
                                              const PublisherConfig& config,
                                              const EndpointConfig& endpoint,
                                              const TlsConfig& tls,
                                              LibmoqPublishStats& out_stats,
                                              LibmoqLiveHandle* live) {
    std::atomic<bool>* cancel = (live != nullptr) ? &live->cancel : nullptr;
    if (cancelled(cancel)) {
        return TransportStatus::success();  // cancelled before connecting: clean stop
    }

    // Phase 1: read ftyp + moov from stdin for track discovery (mirrors the
    // MoqtSession live path so the same fragmentation/grouping is reused).
    StreamingMp4Reader reader;
    std::vector<std::uint8_t> ftyp_bytes;
    std::vector<std::uint8_t> moov_bytes;
    while (ftyp_bytes.empty() || moov_bytes.empty()) {
        const std::size_t bytes_read = reader.read_from(input);
        if (bytes_read == 0 && ftyp_bytes.empty()) {
            return TransportStatus::failure("stdin EOF before ftyp box");
        }
        if (bytes_read == 0 && moov_bytes.empty()) {
            return TransportStatus::failure("stdin EOF before moov box");
        }
        while (auto box = reader.next_box()) {
            if (box->type == "ftyp") {
                ftyp_bytes = std::move(box->bytes);
            } else if (box->type == "moov") {
                moov_bytes = std::move(box->bytes);
                break;
            }
        }
    }

    std::vector<std::uint8_t> init_segment;
    init_segment.reserve(ftyp_bytes.size() + moov_bytes.size());
    init_segment.insert(init_segment.end(), ftyp_bytes.begin(), ftyp_bytes.end());
    init_segment.insert(init_segment.end(), moov_bytes.begin(), moov_bytes.end());

    const std::vector<Mp4Box> init_boxes = parse_mp4_boxes(init_segment);
    const std::vector<TrackDescription> tracks = extract_tracks(init_boxes, init_segment);
    if (tracks.empty()) {
        return TransportStatus::failure("no tracks found in moov box");
    }

    // Per-track CMAF init segments for catalog/init_data (libmoq owns the
    // catalog itself; we only use the per-track init segments here).
    const LiveCatalog live_catalog = build_live_catalog(tracks, init_segment, /*is_live=*/true);
    auto init_for_track = [&](const std::string& name) -> std::vector<std::uint8_t> {
        for (const auto& init : live_catalog.track_initializations) {
            if (init.track_name == name) {
                return init.init_segment;
            }
        }
        return init_segment;  // fall back to the whole ftyp+moov header
    };

    DemandMonitor demand;
    moq_endpoint_t* ep = nullptr;
    moq_media_sender_t* sender = nullptr;
    const TransportStatus setup =
        connect_and_attach(config, endpoint, tls, /*live=*/true, &demand, &ep, &sender);
    if (!setup.ok) {
        return setup;
    }
    // Register the endpoint so disconnect() can interrupt it (cleared by every
    // *_teardown below before the endpoint is destroyed).
    if (live != nullptr) {
        live->set_endpoint(ep);
    }

    const std::uint64_t timeout_us =
        static_cast<std::uint64_t>(config.subscriber_timeout.count()) * 1000000ull;

    std::unordered_map<std::string, moq_media_track_t*> handles;
    for (const auto& td : tracks) {
        if (!is_real_media_track(td)) {
            continue;  // defensive: stdin moov yields only media tracks
        }
        const LibmoqTrackTranslation t = make_libmoq_live_track(td, init_for_track(td.track_name));
        moq_media_track_cfg_t c = t.cfg();
        moq_media_track_t* h = nullptr;
        const moq_result_t rc = moq_media_sender_add_track(sender, &c, &h);
        if (rc != MOQ_OK || h == nullptr) {
            return live_teardown(live, sender, ep,
                                 "add_track failed for '" + t.name + "': " + moq_strerror(rc));
        }
        handles[t.name] = h;
    }

    // Tracks are configured: wait for the initial catalog before streaming.
    const LibmoqReadyOutcome ready = wait_ready(ep, sender, cancel, timeout_us);
    if (ready == LibmoqReadyOutcome::kCancelled) {
        return cancel_teardown(live, sender, ep, LibmoqPublishStats{}, out_stats);
    }
    if (ready != LibmoqReadyOutcome::kReady) {
        return ready_failure_teardown(live, sender, ep, ready);
    }

    // Wait for a real subscriber before consuming stdin: a lazy relay forwards a
    // SUBSCRIBE only when a player subscribes. Until then we do NOT read stdin
    // (ffmpeg blocks on the full pipe), so no live fragments are produced and
    // dropped with nobody watching.
    const LibmoqDemandOutcome demand_out =
        wait_for_media_subscriber(ep, sender, &demand, cancel, timeout_us);
    if (demand_out == LibmoqDemandOutcome::kCancelled) {
        return cancel_teardown(live, sender, ep, LibmoqPublishStats{}, out_stats);
    }
    if (demand_out != LibmoqDemandOutcome::kSubscriber) {
        return demand_failure_teardown(live, sender, ep, demand_out);
    }

    // Phase 2: stream moof+mdat fragments until stdin EOF. libmoq owns the
    // network thread, so we read and write on this thread; the sender enqueues
    // and drains on its own thread. Grouping mirrors the MoqtSession live path:
    // a new shared group begins at each video keyframe; object ids reset per
    // group, per track.
    const moq_alloc_t* alloc = moq_alloc_default();
    LibmoqPublishStats stats;
    std::set<std::pair<std::string, std::size_t>> groups;
    std::vector<std::uint8_t> pending_moof;
    std::size_t shared_group_id = 0;
    std::map<std::string, std::size_t> object_id_in_group;
    bool first_keyframe_seen = false;

    for (;;) {
        if (cancelled(cancel)) {
            stats.groups_published = static_cast<std::uint64_t>(groups.size());
            return cancel_teardown(live, sender, ep, stats, out_stats);
        }
        const std::size_t bytes_read = reader.read_from(input);
        while (auto box = reader.next_box()) {
            if (box->type == "moof") {
                pending_moof = std::move(box->bytes);
                continue;
            }
            if (box->type != "mdat") {
                continue;  // skip styp/free/etc.
            }
            if (pending_moof.empty()) {
                continue;  // mdat without a preceding moof
            }

            MediaFragment fragment;
            try {
                fragment = build_live_fragment(pending_moof, box->bytes, tracks, 0);
            } catch (const std::exception&) {
                pending_moof.clear();
                continue;
            }
            pending_moof.clear();

            if (fragment.is_video_keyframe) {
                if (first_keyframe_seen) {
                    ++shared_group_id;
                }
                first_keyframe_seen = true;
                object_id_in_group.clear();
            }
            if (!first_keyframe_seen) {
                continue;  // drop fragments before the first keyframe (no IDR)
            }
            fragment.group_id = shared_group_id;
            fragment.object_id = object_id_in_group[fragment.track_name]++;

            const auto hit = handles.find(fragment.track_name);
            if (hit == handles.end()) {
                continue;  // fragment for an undiscovered track
            }

            const LibmoqObjectTranslation translated = make_libmoq_live_object(fragment);
            const moq_result_t wrc =
                write_live_object(sender, hit->second, alloc, translated, stats, groups);
            // Only WOULD_BLOCK is a tolerated live drop (drop-to-keyframe policy);
            // any other write error (INVAL/NOMEM/WRONG_STATE/CLOSED/...) is fatal.
            if (wrc != MOQ_OK && wrc != MOQ_ERR_WOULD_BLOCK) {
                return live_teardown(live, sender, ep,
                                     std::string("media write failed: ") + moq_strerror(wrc));
            }
        }

        if (bytes_read == 0) {
            break;  // stdin EOF
        }
    }

    stats.groups_published = static_cast<std::uint64_t>(groups.size());

    for (const auto& entry : handles) {
        int erc = MOQ_OK;
        const LibmoqRetryOutcome eout =
            retry_end_track(sender, ep, entry.second, cancel, timeout_us, &erc);
        if (eout == LibmoqRetryOutcome::kCancelled) {
            return cancel_teardown(live, sender, ep, stats, out_stats);
        }
        if (eout != LibmoqRetryOutcome::kOk) {
            return retry_failure_teardown(live, sender, ep, eout, erc, "end_track");
        }
    }

    drain_sender(ep, sender, timeout_us, cancel);

    out_stats = stats;
    return live_teardown(live, sender, ep, "");
}

TransportStatus publish_live_srt_via_libmoq(std::vector<LiveSrtCallerRuntimeConfig> srt_callers,
                                            const PublisherConfig& config,
                                            const EndpointConfig& endpoint,
                                            const TlsConfig& tls,
                                            LibmoqPublishStats& out_stats,
                                            LibmoqLiveHandle* live) {
    std::atomic<bool>* cancel = (live != nullptr) ? &live->cancel : nullptr;
    if (srt_callers.empty()) {
        return TransportStatus::failure("no SRT callers configured");
    }
    if (cancelled(cancel)) {
        return TransportStatus::success();  // cancelled before starting: clean stop
    }

    // Fragment queue filled by the SRT ingest worker threads (via the sink) and
    // drained on this thread into the media sender.
    struct LiveQueue {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<MediaFragment> fragments;
        bool eof = false;
    };
    auto queue = std::make_shared<LiveQueue>();
    std::atomic<bool> stop_requested{false};

    LiveSrtIngestManager manager(
        std::move(srt_callers),
        [queue](MediaFragment&& fragment) {
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                queue->fragments.push_back(std::move(fragment));
            }
            queue->cv.notify_one();
        },
        stop_requested);

    const TransportStatus start = manager.start();
    if (!start.ok) {
        return start;  // start() failed: nothing was spawned, nothing to join
    }

    const std::vector<TrackDescription>& tracks = manager.bootstrap().tracks;
    if (tracks.empty()) {
        stop_requested = true;
        manager.join();
        return TransportStatus::failure("no live tracks available from SRT source");
    }

    // Background thread: when the source ends (all workers join), mark EOF so the
    // drain loop can finish. Spawned only after a successful start with tracks.
    std::thread join_thread([&manager, queue]() {
        manager.join();
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->eof = true;
        }
        queue->cv.notify_all();
    });

    // From here, every exit path must stop the source and join the worker/join
    // threads. This guard runs at scope exit -- after any ep/sender teardown
    // below -- so the SRT threads are always stopped and joined.
    struct SrtGuard {
        std::atomic<bool>& stop;
        std::thread& join_thread;
        ~SrtGuard() {
            stop = true;
            if (join_thread.joinable()) {
                join_thread.join();
            }
        }
    } srt_guard{stop_requested, join_thread};

    // Per-track CMAF init segments derived from the synthetic init segment.
    const std::vector<std::uint8_t> synthetic_init =
        LiveSrtIngestManager::build_synthetic_init_segment(tracks);
    const LiveCatalog live_catalog = build_live_catalog(tracks, synthetic_init, /*is_live=*/true);
    auto init_for_track = [&](const std::string& name) -> std::vector<std::uint8_t> {
        for (const auto& init : live_catalog.track_initializations) {
            if (init.track_name == name) {
                return init.init_segment;
            }
        }
        return synthetic_init;
    };

    DemandMonitor demand;
    moq_endpoint_t* ep = nullptr;
    moq_media_sender_t* sender = nullptr;
    const TransportStatus setup =
        connect_and_attach(config, endpoint, tls, /*live=*/true, &demand, &ep, &sender);
    if (!setup.ok) {
        return setup;  // srt_guard stops + joins the manager
    }
    if (live != nullptr) {
        live->set_endpoint(ep);  // let disconnect() interrupt this endpoint
    }

    const std::uint64_t timeout_us =
        static_cast<std::uint64_t>(config.subscriber_timeout.count()) * 1000000ull;

    std::unordered_map<std::string, moq_media_track_t*> handles;
    for (const auto& td : tracks) {
        if (!is_real_media_track(td)) {
            continue;  // skip any synthetic/metadata track in the bootstrap
        }
        const LibmoqTrackTranslation t = make_libmoq_live_track(td, init_for_track(td.track_name));
        moq_media_track_cfg_t c = t.cfg();
        moq_media_track_t* h = nullptr;
        const moq_result_t rc = moq_media_sender_add_track(sender, &c, &h);
        if (rc != MOQ_OK || h == nullptr) {
            return live_teardown(live, sender, ep,
                                 "add_track failed for '" + t.name + "': " + moq_strerror(rc));
        }
        handles[t.name] = h;
    }

    // Tracks are configured: wait for the initial catalog before streaming.
    const LibmoqReadyOutcome ready = wait_ready(ep, sender, cancel, timeout_us);
    if (ready == LibmoqReadyOutcome::kCancelled) {
        return cancel_teardown(live, sender, ep, LibmoqPublishStats{}, out_stats);
    }
    if (ready != LibmoqReadyOutcome::kReady) {
        return ready_failure_teardown(live, sender, ep, ready);
    }

    // Drain the fragment queue until the source ends. The SRT manager assigns
    // each fragment's group_id/object_id, so make_libmoq_live_object maps them
    // directly (no keyframe-grouping needed here, unlike the stdin path).
    const moq_alloc_t* alloc = moq_alloc_default();
    LibmoqPublishStats stats;
    std::set<std::pair<std::string, std::size_t>> groups;
    bool fatal = false;
    std::string fatal_msg;
    for (;;) {
        if (cancelled(cancel)) {
            stats.groups_published = static_cast<std::uint64_t>(groups.size());
            return cancel_teardown(live, sender, ep, stats, out_stats);
        }
        if (moq_media_sender_is_fatal(sender) || moq_endpoint_is_fatal(ep)) {
            fatal = true;
            fatal_msg = "endpoint/sender became fatal";
            break;
        }
        MediaFragment fragment;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(queue->mutex);
            queue->cv.wait_for(lock, std::chrono::milliseconds(50), [&queue] {
                return !queue->fragments.empty() || queue->eof;
            });
            if (!queue->fragments.empty()) {
                fragment = std::move(queue->fragments.front());
                queue->fragments.pop_front();
                have = true;
            } else if (queue->eof) {
                break;  // source ended and queue drained
            }
        }
        if (!have) {
            continue;  // timeout/spurious wake -- re-check fatal and queue
        }

        const auto hit = handles.find(fragment.track_name);
        if (hit == handles.end()) {
            continue;  // fragment for an untracked stream
        }
        // Lazy relay: with no media subscriber, DROP this live fragment instead
        // of buffering it. We still pop every fragment above, so the SRT queue
        // stays bounded while we wait for a player to subscribe; once demand
        // exists we start writing the live edge.
        if (!moq_media_sender_has_media_subscriber(sender)) {
            continue;
        }
        const LibmoqObjectTranslation translated = make_libmoq_live_object(fragment);
        const moq_result_t wrc =
            write_live_object(sender, hit->second, alloc, translated, stats, groups);
        // Only WOULD_BLOCK is a tolerated live drop (drop-to-keyframe policy);
        // any other write error (INVAL/NOMEM/WRONG_STATE/CLOSED/...) is fatal.
        if (wrc != MOQ_OK && wrc != MOQ_ERR_WOULD_BLOCK) {
            fatal = true;
            fatal_msg = std::string("media write failed: ") + moq_strerror(wrc);
            break;
        }
    }

    if (fatal) {
        return live_teardown(live, sender, ep, fatal_msg);
    }

    stats.groups_published = static_cast<std::uint64_t>(groups.size());

    for (const auto& entry : handles) {
        int erc = MOQ_OK;
        const LibmoqRetryOutcome eout =
            retry_end_track(sender, ep, entry.second, cancel, timeout_us, &erc);
        if (eout == LibmoqRetryOutcome::kCancelled) {
            return cancel_teardown(live, sender, ep, stats, out_stats);
        }
        if (eout != LibmoqRetryOutcome::kOk) {
            return retry_failure_teardown(live, sender, ep, eout, erc, "end_track");
        }
    }

    drain_sender(ep, sender, timeout_us, cancel);

    out_stats = stats;
    return live_teardown(live, sender, ep, "");
}

TransportStatus publish_live_objects_via_libmoq(const LiveObjectSource& source,
                                                const PublisherConfig& config,
                                                const EndpointConfig& endpoint,
                                                const TlsConfig& tls,
                                                LibmoqPublishStats& out_stats,
                                                LibmoqLiveHandle* live) {
    std::atomic<bool>* cancel = (live != nullptr) ? &live->cancel : nullptr;
    if (source.tracks.empty()) {
        return TransportStatus::failure("live object source has no tracks");
    }
    if (!source.next_object) {
        return TransportStatus::failure("live object source has no object reader");
    }
    if (cancelled(cancel)) {
        return TransportStatus::success();  // cancelled before connecting: clean stop
    }

    DemandMonitor demand;
    moq_endpoint_t* ep = nullptr;
    moq_media_sender_t* sender = nullptr;
    const TransportStatus setup =
        connect_and_attach(config, endpoint, tls, /*live=*/true, &demand, &ep, &sender);
    if (!setup.ok) {
        return setup;
    }
    if (live != nullptr) {
        live->set_endpoint(ep);  // let disconnect() interrupt this endpoint
    }

    const std::uint64_t timeout_us =
        static_cast<std::uint64_t>(config.subscriber_timeout.count()) * 1000000ull;

    std::unordered_map<std::string, moq_media_track_t*> handles;
    for (const auto& track : source.tracks) {
        const LibmoqTrackTranslation t = make_libmoq_live_object_track(track);
        moq_media_track_cfg_t c = t.cfg();
        moq_media_track_t* h = nullptr;
        const moq_result_t rc = moq_media_sender_add_track(sender, &c, &h);
        if (rc != MOQ_OK || h == nullptr) {
            return live_teardown(live, sender, ep,
                                 "add_track failed for '" + t.name + "': " + moq_strerror(rc));
        }
        handles[t.name] = h;
    }

    // Tracks are configured: wait for the initial catalog before writing.
    const LibmoqReadyOutcome ready = wait_ready(ep, sender, cancel, timeout_us);
    if (ready == LibmoqReadyOutcome::kCancelled) {
        return cancel_teardown(live, sender, ep, LibmoqPublishStats{}, out_stats);
    }
    if (ready != LibmoqReadyOutcome::kReady) {
        return ready_failure_teardown(live, sender, ep, ready);
    }

    // Wait for a real subscriber before pulling from the source: a lazy relay
    // forwards a SUBSCRIBE only when a player subscribes, so without this the
    // app's next_object() would be consumed (and its objects dropped) with
    // nobody watching.
    const LibmoqDemandOutcome demand_out =
        wait_for_media_subscriber(ep, sender, &demand, cancel, timeout_us);
    if (demand_out == LibmoqDemandOutcome::kCancelled) {
        return cancel_teardown(live, sender, ep, LibmoqPublishStats{}, out_stats);
    }
    if (demand_out != LibmoqDemandOutcome::kSubscriber) {
        return demand_failure_teardown(live, sender, ep, demand_out);
    }

    const moq_alloc_t* alloc = moq_alloc_default();
    LibmoqPublishStats stats;
    std::set<std::pair<std::string, std::size_t>> groups;
    for (;;) {
        if (cancelled(cancel)) {
            stats.groups_published = static_cast<std::uint64_t>(groups.size());
            return cancel_teardown(live, sender, ep, stats, out_stats);
        }
        std::optional<LiveObject> next;
        try {
            next = source.next_object();
        } catch (const std::runtime_error& error) {
            // A catalog-build refusal (e.g. a protected track with no pssh
            // anywhere in the init segment) throws from inside next_object()
            // rather than returning nullopt. Report and tear down through the
            // normal path instead of letting it unwind past every
            // live_teardown() call: uncaught, it would skip out_stats and
            // leave ep/sender un-torn-down, only to be caught by main.cpp's
            // top-level handler far from any connection state.
            return live_teardown(live, sender, ep,
                                 std::string("live object source failed: ") + error.what());
        }
        if (!next.has_value()) {
            break;  // source exhausted
        }
        const LiveObject& object = *next;
        const auto hit = handles.find(object.track_name);
        if (hit == handles.end()) {
            return live_teardown(live, sender, ep,
                                 "live object references undeclared track '" + object.track_name +
                                     "'");
        }
        const LibmoqObjectTranslation translated = make_libmoq_live_source_object(object);
        const moq_result_t wrc =
            write_live_object(sender, hit->second, alloc, translated, stats, groups);
        // Only WOULD_BLOCK is a tolerated live drop (drop-to-keyframe policy);
        // any other write error is fatal.
        if (wrc != MOQ_OK && wrc != MOQ_ERR_WOULD_BLOCK) {
            return live_teardown(live, sender, ep,
                                 std::string("media write failed: ") + moq_strerror(wrc));
        }
    }

    stats.groups_published = static_cast<std::uint64_t>(groups.size());

    for (const auto& entry : handles) {
        int erc = MOQ_OK;
        const LibmoqRetryOutcome eout =
            retry_end_track(sender, ep, entry.second, cancel, timeout_us, &erc);
        if (eout == LibmoqRetryOutcome::kCancelled) {
            return cancel_teardown(live, sender, ep, stats, out_stats);
        }
        if (eout != LibmoqRetryOutcome::kOk) {
            return retry_failure_teardown(live, sender, ep, eout, erc, "end_track");
        }
    }

    drain_sender(ep, sender, timeout_us, cancel);

    out_stats = stats;
    return live_teardown(live, sender, ep, "");
}

}  // namespace openmoq::publisher::transport

#endif  // OPENMOQ_HAS_LIBMOQ
