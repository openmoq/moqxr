#pragma once

#include "openmoq/publisher/mp4_box.h"

#include <cstdint>
#include <map>
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
};

// The root catalog object (MSF section 5.1).
struct MsfCatalog {
    std::string version = "1";                    // 5.1.1, a String
    std::optional<std::uint64_t> generated_at_ms; // 5.1.2
    std::optional<bool> is_complete;              // 5.1.3
    std::vector<MsfTrack> tracks;                 // 5.1.4
    std::vector<MsfTrack> publish_tracks;         // 5.1.5
    std::vector<MsfInitData> init_data_list;      // 5.1.7, emitted after tracks
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

}  // namespace openmoq::publisher
