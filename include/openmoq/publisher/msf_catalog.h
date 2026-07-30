#pragma once

#include "openmoq/publisher/mp4_box.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher {

// One entry of the root initDataList (MSF section 5.1.7). Only the "inline"
// type is defined in version 1; data is Base64-encoded initialization data.
struct MsfInitData {
    std::string id;
    std::string type = "inline";
    std::string data;
};

// Target buffer object (MSF section 5.2.9). All keys are optional. Mutually
// exclusive with targetLatency within a single track.
struct MsfBuffers {
    std::optional<std::uint64_t> target_ms;
    std::optional<std::uint64_t> min_ms;
    std::optional<std::uint64_t> max_ms;
};

// A URL with an optional type, used by the DRM system fields in CMSF 4.1.1.4.
// Declared before MsfContentProtection, which holds these by value.
//
// Note: CMSF 4.1.1.4.4 describes an Authorization URL but never names its JSON
// key, and the draft's examples show only laURL and certURL. It is therefore
// NOT modelled here -- emitting an invented key would be guessing at the spec,
// and nothing in DrmSystemConfig could populate it anyway.
struct MsfUrlEntry {
    std::string url;
    std::optional<std::string> type;
};

// One DRM system configuration at the catalog root (CMSF 4.1.1). Tracks
// reference these by ref_id; the data is never duplicated on a track.
struct MsfContentProtection {
    std::string ref_id;                       // 4.1.1.1, required
    std::vector<std::string> default_kids;    // 4.1.1.2, required
    std::string scheme;                       // 4.1.1.3, "cenc" or "cbcs"
    std::string system_id;                    // 4.1.1.4.1, required
    std::optional<MsfUrlEntry> la_url;        // 4.1.1.4.2
    std::optional<MsfUrlEntry> cert_url;      // 4.1.1.4.3
    std::optional<std::string> pssh_base64;   // 4.1.1.4.5
    std::optional<std::string> robustness;    // 4.1.1.4.6
};

// A track object (MSF section 5.2). Field names mirror the spec exactly.
// std::optional models absence so the serializer never emits a field the
// drafts say MUST NOT appear.
struct MsfTrack {
    std::string name;                                  // 5.2.3, required
    std::optional<std::string> name_space;             // 5.2.2 ("namespace")
    std::string packaging;                             // 5.2.4, required
    std::optional<std::string> role;                   // 5.2.6
    bool is_live = false;                              // 5.2.7, required
    std::optional<std::uint64_t> target_latency_ms;    // 5.2.8
    std::optional<MsfBuffers> buffers;                 // 5.2.9
    std::optional<std::string> label;                  // 5.2.10
    std::optional<std::uint32_t> render_group;         // 5.2.11
    std::optional<std::uint32_t> alt_group;            // 5.2.12
    std::optional<std::string> init_ref;               // 5.2.13
    std::vector<std::string> depends;                  // 5.2.14
    std::optional<std::string> codec;                  // 5.2.18
    std::optional<std::string> mime_type;              // 5.2.19
    std::optional<double> framerate;                   // 5.2.20
    std::optional<std::uint32_t> timescale;            // 5.2.21
    std::optional<std::uint64_t> bitrate;              // 5.2.22, MUST for a/v
    std::optional<std::uint64_t> avg_bitrate;          // 5.2.23
    std::optional<std::uint64_t> max_gop_duration_ms;  // 5.2.24
    std::optional<std::uint64_t> max_group_duration_ms;// 5.2.25
    std::optional<std::uint32_t> width;                // 5.2.26
    std::optional<std::uint32_t> height;               // 5.2.27
    std::optional<std::uint32_t> samplerate;           // 5.2.28
    std::optional<std::string> channel_config;         // 5.2.29, a String
    std::optional<std::string> lang;                   // 5.2.32
    std::optional<std::uint64_t> track_duration_ms;    // 5.2.35
    std::optional<std::string> event_type;             // 5.2.5, eventtimeline only

    // CMSF section 3.5.2.
    std::optional<std::uint32_t> max_grp_sap_starting_type;
    std::optional<std::uint32_t> max_obj_sap_starting_type;

    // Producer-defined fields. MSF section 5 permits these provided the names
    // do not collide with spec field names; the serializer enforces that.
    // Values are raw JSON, so a string value must arrive already quoted.
    std::map<std::string, std::string> custom_fields;

    std::vector<std::string> content_protection_ref_ids;     // 4.1.2
};

// One operation in a delta update (MSF section 5.1.6). This version emits
// "add" and "remove" only. "clone" is deliberately unimplemented: it applies
// when a new track matches an existing one on every field except name, which
// no producer in this project generates.
struct MsfDeltaOp {
    std::string op;
    std::vector<MsfTrack> tracks;
};

// The root catalog object (MSF section 5.1).
struct MsfCatalog {
    std::string version = "1";                    // 5.1.1, a String
    std::optional<std::uint64_t> generated_at_ms; // 5.1.2
    std::optional<bool> is_complete;              // 5.1.3
    std::vector<MsfContentProtection> content_protections;   // 4.1.1
    std::vector<MsfTrack> tracks;                 // 5.1.4
    std::vector<MsfTrack> publish_tracks;         // 5.1.5
    std::vector<MsfInitData> init_data_list;      // 5.1.7, emitted after tracks

    // When non-empty this catalog is a DELTA: section 5.3 requires it to
    // carry neither tracks nor version.
    std::vector<MsfDeltaOp> delta_update;
};

// Serialize to a JSON catalog document. Throws std::runtime_error when a
// draft invariant is violated; the message names the offending track.
std::string serialize_catalog(const MsfCatalog& catalog);

// Resolve the bitrate MSF section 5.2.22 requires for audio and video, in
// precedence order: the track's btrt value, then a configured override, then
// a value computed from sample sizes, then a codec-class default. The default
// keeps the catalog conformant on live paths where no measurement exists; the
// caller is expected to log when it is used.
std::uint64_t resolve_bitrate(const TrackDescription& track,
                              std::optional<std::uint64_t> configured,
                              std::uint64_t computed);

// True when resolve_bitrate would fall through to a codec-class default, so
// callers can warn the operator that the published figure is an estimate.
bool bitrate_is_estimated(const TrackDescription& track,
                          std::optional<std::uint64_t> configured,
                          std::uint64_t computed);

// Build the MSF track object for a parsed MP4 track. Shared by every emitter
// so the four publish paths cannot drift apart again. The caller still sets
// init_ref, alt_group, and the CMSF max SAP starting types, which depend on
// catalog-level and segmentation context this function does not see.
MsfTrack make_msf_track(const TrackDescription& track,
                        bool is_live,
                        std::optional<std::uint64_t> configured_bitrate = std::nullopt,
                        std::uint64_t computed_bitrate = 0);

// Attach a Base64-encoded init segment to a track: sets track.init_ref and
// appends the matching MsfInitData entry to the catalog's initDataList,
// using "<track_name>-init" as the shared id convention. Centralizes the
// initRef/initDataList wiring that every MP4 emitter otherwise repeats.
void attach_init_data(MsfCatalog& catalog, MsfTrack& track, std::string_view track_name, std::string base64_data);

// Add or reuse a root contentProtections entry for each DRM system carrying
// this track's protection, and point the track at them by refID. Systems
// already present (matched by system_id and scheme) are reused rather than
// duplicated, so two tracks sharing a KID share entries.
void attach_content_protection(MsfCatalog& catalog,
                               MsfTrack& track,
                               const CencTrackProtection& protection,
                               const std::vector<CencSystem>& systems);

// How a live broadcast ends (MSF section 11.3).
enum class EndBroadcastMode {
    // The stream becomes a VOD asset: isLive false, trackDuration added.
    kConvertToVod,
    // The stream ends permanently: isComplete true, empty tracks array.
    kTerminate,
};

// Build the final independent catalog for a broadcast that is ending.
//
// kConvertToVod clears is_live and applies track_durations_ms in one rebuild.
// Both must change together: validate_track rejects a track that is live and
// also carries a duration, so flipping one field at a time would throw.
// generatedAt is dropped because MSF 5.1.2 says it SHOULD NOT appear when
// isLive is false.
//
// kTerminate returns a catalog with isComplete true and no tracks at all;
// track_durations_ms is ignored.
MsfCatalog make_end_broadcast_catalog(const MsfCatalog& current,
                                      EndBroadcastMode mode,
                                      const std::map<std::string, std::uint64_t>& track_durations_ms);

// One catalog object ready for the wire. MSF section 5 requires all catalog
// objects to map to MOQT sub-group 0.
struct CatalogObject {
    std::uint64_t group_id = 0;
    std::uint64_t object_id = 0;
    std::uint64_t subgroup_id = 0;
    std::string payload;
};

// Owns the catalog track's group and object counters and the last published
// catalog, and decides what to emit (MSF section 5).
//
// Object 0 of every group holds a full independent catalog. Producing an
// independent catalog always starts a new group.
//
// Thread safety: Publisher::end_broadcast() is documented to invoke
// MoqtSession::end_broadcast() (and, transitively, this class's own
// end_broadcast()) without holding Publisher's state_mutex_, specifically so
// it can run concurrently with an in-progress publish()/publish_live() call
// on the same session -- see publisher_api.h and moqt_session.h. That in-
// progress call may be inside publish() reading `last_` (via catalogs_equal)
// at the same moment end_broadcast() reassigns it. mutex_ serializes every
// entry point below so that read and the reassignment can never overlap.
class CatalogPublisher {
public:
    // Emit whatever is needed to move subscribers to `desired`. Returns an
    // empty vector when nothing changed. Throws std::runtime_error if the
    // broadcast has already ended.
    std::vector<CatalogObject> publish(const MsfCatalog& desired);

    // Emit the final catalog for a broadcast that is ending (MSF 11.3).
    // After this, publish() throws: section 5.1.3 forbids removing
    // isComplete, and 5.2.7 forbids a true isLive following a false one.
    std::vector<CatalogObject> end_broadcast(
        EndBroadcastMode mode,
        const std::map<std::string, std::uint64_t>& track_durations_ms);

    // Re-emit the last published catalog as a fresh independent copy in a new
    // group, for section 5's cache-expiry republication. Returns an empty
    // vector if nothing has been published yet.
    std::vector<CatalogObject> force_independent();

    bool ended() const;

    // Section 5.3: producers publishing frequent deltas SHOULD periodically
    // publish a new independent catalog to bound the delta processing a
    // joining subscriber must perform. Default 8.
    void set_max_deltas_per_group(std::size_t max_deltas) { max_deltas_per_group_ = max_deltas; }

    // Reset all per-broadcast state (the last published catalog, the group
    // and object counters, and the ended flag) back to how a freshly
    // constructed CatalogPublisher starts. max_deltas_per_group_ is left
    // alone, since it is a standing configuration knob rather than
    // per-broadcast state. A caller that reuses one MoqtSession instance
    // across more than one publish()/publish_live() call must call this
    // first, or the second broadcast's first publish() would see `last_`
    // already set from the previous broadcast and (via catalogs_equal)
    // silently send no catalog at all to the new broadcast's subscribers.
    void reset();

private:
    CatalogObject emit_independent(const MsfCatalog& catalog);

    mutable std::mutex mutex_;
    std::optional<MsfCatalog> last_;
    std::uint64_t next_group_id_ = 0;
    std::uint64_t next_object_id_ = 0;
    bool ended_ = false;
    std::size_t max_deltas_per_group_ = 8;
    std::size_t deltas_in_group_ = 0;
};

}  // namespace openmoq::publisher
