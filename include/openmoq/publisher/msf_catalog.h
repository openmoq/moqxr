#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
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

}  // namespace openmoq::publisher
