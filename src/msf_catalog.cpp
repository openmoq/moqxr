#include "openmoq/publisher/msf_catalog.h"

#include <cmath>
#include <iomanip>
#include <ios>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace openmoq::publisher {

namespace {

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec;
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

// Emit a JSON number without a trailing ".0" for whole values, so framerate 30
// serializes as 30 rather than 30.000000.
std::string json_number(double value) {
    std::ostringstream out;
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) < 0.0005) {
        out << static_cast<long long>(rounded);
    } else {
        out << std::fixed << std::setprecision(3) << value;
    }
    return out.str();
}

// Comma bookkeeping for a JSON object or array under construction.
class JsonSeq {
public:
    explicit JsonSeq(std::ostringstream& out) : out_(out) {}

    void separate() {
        if (!first_) {
            out_ << ',';
        }
        first_ = false;
    }

private:
    std::ostringstream& out_;
    bool first_ = true;
};

void write_string(std::ostringstream& out, JsonSeq& seq, std::string_view key, std::string_view value) {
    seq.separate();
    out << '"' << json_escape(key) << "\":\"" << json_escape(value) << '"';
}

void write_raw(std::ostringstream& out, JsonSeq& seq, std::string_view key, std::string_view raw) {
    seq.separate();
    out << '"' << json_escape(key) << "\":" << raw;
}

void write_uint(std::ostringstream& out, JsonSeq& seq, std::string_view key, std::uint64_t value) {
    write_raw(out, seq, key, std::to_string(value));
}

void write_bool(std::ostringstream& out, JsonSeq& seq, std::string_view key, bool value) {
    write_raw(out, seq, key, value ? "true" : "false");
}

void write_string_array(std::ostringstream& out,
                        JsonSeq& seq,
                        std::string_view key,
                        const std::vector<std::string>& values) {
    seq.separate();
    out << '"' << json_escape(key) << "\":[";
    JsonSeq inner(out);
    for (const auto& value : values) {
        inner.separate();
        out << '"' << json_escape(value) << '"';
    }
    out << ']';
}

bool is_media_role(const MsfTrack& track) {
    return track.role.has_value() && (*track.role == "video" || *track.role == "audio");
}

// MSF and CMSF invariants that a malformed caller could otherwise publish.
void validate_track(const MsfTrack& track) {
    const std::string where = " (track \"" + track.name + "\")";

    if (track.name.empty()) {
        throw std::runtime_error("MSF catalog track requires a name");
    }
    if (track.packaging.empty()) {
        throw std::runtime_error("MSF catalog requires a packaging value" + where);
    }
    // Section 5.2.22: bitrate MUST be specified for audio and video tracks.
    if (is_media_role(track) && !track.bitrate.has_value()) {
        throw std::runtime_error("MSF catalog requires bitrate for audio and video tracks" + where);
    }
    // Section 5.2.5: eventType is required for, and restricted to, eventtimeline.
    if (track.packaging == "eventtimeline" && !track.event_type.has_value()) {
        throw std::runtime_error("MSF catalog requires eventType for eventtimeline packaging" + where);
    }
    if (track.packaging != "eventtimeline" && track.event_type.has_value()) {
        throw std::runtime_error("MSF catalog forbids eventType outside eventtimeline packaging" + where);
    }
    // Section 5.2.35: trackDuration MUST NOT be included when isLive is true.
    if (track.is_live && track.track_duration_ms.has_value()) {
        throw std::runtime_error("MSF catalog forbids trackDuration on a live track" + where);
    }
    // Section 5.2.28 and 5.2.29: both MUST accompany an audio codec.
    if (track.role.has_value() && *track.role == "audio") {
        if (!track.samplerate.has_value() || !track.channel_config.has_value()) {
            throw std::runtime_error("MSF catalog requires samplerate and channelConfig for audio" + where);
        }
    }
    // Sections 5.2.8 and 5.2.9: targetLatency MUST NOT be present if buffers is.
    if (track.target_latency_ms.has_value() && track.buffers.has_value()) {
        throw std::runtime_error("MSF catalog forbids both targetLatency and buffers" + where);
    }
    // Section 5: custom field names MUST NOT collide with spec field names.
    static const std::set<std::string> kSpecFieldNames = {
        "name", "namespace", "packaging", "eventType", "role", "isLive",
        "targetLatency", "buffers", "label", "renderGroup", "altGroup",
        "initRef", "depends", "codec", "mimeType", "framerate", "timescale",
        "bitrate", "avgBitrate", "maxGopDuration", "maxGroupDuration", "width",
        "height", "samplerate", "channelConfig", "lang", "trackDuration",
        "maxGrpSapStartingType", "maxObjSapStartingType", "temporalId",
        "spatialId", "displayWidth", "displayHeight", "parentName",
        "parentNamespace", "template", "authInfo", "accessibility",
        "encryptionScheme", "cipherSuite", "keyId", "trackBaseKey",
        "connectionUri", "token", "contentProtectionRefIDs",
    };
    for (const auto& [key, value] : track.custom_fields) {
        if (kSpecFieldNames.count(key) != 0) {
            throw std::runtime_error("MSF catalog custom field \"" + key +
                                     "\" collides with a spec field name" + where);
        }
        if (value.empty()) {
            throw std::runtime_error("MSF catalog custom field \"" + key +
                                     "\" has an empty raw JSON value" + where);
        }
    }
}

// Catalog-wide invariants that cannot be checked from a single track.
void validate_catalog(const MsfCatalog& catalog) {
    std::set<std::string> init_ids;
    for (const auto& entry : catalog.init_data_list) {
        if (!init_ids.insert(entry.id).second) {
            throw std::runtime_error("MSF catalog has a duplicate initDataList id \"" + entry.id + "\"");
        }
    }

    // Section 5.2.3: track names MUST be unique per namespace.
    // Both tracks and publishTracks must be checked together for uniqueness.
    std::set<std::pair<std::string, std::string>> seen;

    for (const auto& track : catalog.tracks) {
        const std::string ns = track.name_space.value_or(std::string{});
        if (!seen.insert({ns, track.name}).second) {
            throw std::runtime_error("MSF catalog has a duplicate track name \"" + track.name +
                                     "\" within one namespace");
        }
        // Section 5.2.13: initRef points at an id in the initDataList.
        if (track.init_ref.has_value() && init_ids.count(*track.init_ref) == 0) {
            throw std::runtime_error("MSF catalog initRef \"" + *track.init_ref +
                                     "\" has no matching initDataList entry (track \"" + track.name + "\")");
        }
    }

    // Section 5.1.5: publishTracks follow the same structure as tracks.
    for (const auto& track : catalog.publish_tracks) {
        const std::string ns = track.name_space.value_or(std::string{});
        if (!seen.insert({ns, track.name}).second) {
            throw std::runtime_error("MSF catalog has a duplicate track name \"" + track.name +
                                     "\" within one namespace");
        }
        // Section 5.2.13: initRef points at an id in the initDataList.
        if (track.init_ref.has_value() && init_ids.count(*track.init_ref) == 0) {
            throw std::runtime_error("MSF catalog initRef \"" + *track.init_ref +
                                     "\" has no matching initDataList entry (track \"" + track.name + "\")");
        }
    }
}

void write_track(std::ostringstream& out, const MsfTrack& track) {
    validate_track(track);

    out << '{';
    JsonSeq seq(out);

    write_string(out, seq, "name", track.name);
    if (track.name_space.has_value()) {
        write_string(out, seq, "namespace", *track.name_space);
    }
    write_string(out, seq, "packaging", track.packaging);
    if (track.event_type.has_value()) {
        write_string(out, seq, "eventType", *track.event_type);
    }
    if (track.role.has_value()) {
        write_string(out, seq, "role", *track.role);
    }
    write_bool(out, seq, "isLive", track.is_live);
    if (track.target_latency_ms.has_value()) {
        write_uint(out, seq, "targetLatency", *track.target_latency_ms);
    }
    if (track.buffers.has_value()) {
        seq.separate();
        out << "\"buffers\":{";
        JsonSeq buf(out);
        if (track.buffers->target_ms.has_value()) {
            write_uint(out, buf, "target", *track.buffers->target_ms);
        }
        if (track.buffers->min_ms.has_value()) {
            write_uint(out, buf, "min", *track.buffers->min_ms);
        }
        if (track.buffers->max_ms.has_value()) {
            write_uint(out, buf, "max", *track.buffers->max_ms);
        }
        out << '}';
    }
    if (track.label.has_value()) {
        write_string(out, seq, "label", *track.label);
    }
    if (track.render_group.has_value()) {
        write_uint(out, seq, "renderGroup", *track.render_group);
    }
    if (track.alt_group.has_value()) {
        write_uint(out, seq, "altGroup", *track.alt_group);
    }
    if (track.init_ref.has_value()) {
        write_string(out, seq, "initRef", *track.init_ref);
    }
    if (!track.depends.empty()) {
        write_string_array(out, seq, "depends", track.depends);
    }
    if (track.codec.has_value()) {
        write_string(out, seq, "codec", *track.codec);
    }
    if (track.mime_type.has_value()) {
        write_string(out, seq, "mimeType", *track.mime_type);
    }
    if (track.framerate.has_value()) {
        write_raw(out, seq, "framerate", json_number(*track.framerate));
    }
    if (track.timescale.has_value()) {
        write_uint(out, seq, "timescale", *track.timescale);
    }
    if (track.bitrate.has_value()) {
        write_uint(out, seq, "bitrate", *track.bitrate);
    }
    if (track.avg_bitrate.has_value()) {
        write_uint(out, seq, "avgBitrate", *track.avg_bitrate);
    }
    if (track.max_gop_duration_ms.has_value()) {
        write_uint(out, seq, "maxGopDuration", *track.max_gop_duration_ms);
    }
    if (track.max_group_duration_ms.has_value()) {
        write_uint(out, seq, "maxGroupDuration", *track.max_group_duration_ms);
    }
    if (track.width.has_value()) {
        write_uint(out, seq, "width", *track.width);
    }
    if (track.height.has_value()) {
        write_uint(out, seq, "height", *track.height);
    }
    if (track.samplerate.has_value()) {
        write_uint(out, seq, "samplerate", *track.samplerate);
    }
    if (track.channel_config.has_value()) {
        write_string(out, seq, "channelConfig", *track.channel_config);
    }
    if (track.lang.has_value()) {
        write_string(out, seq, "lang", *track.lang);
    }
    if (track.track_duration_ms.has_value()) {
        write_uint(out, seq, "trackDuration", *track.track_duration_ms);
    }
    if (track.max_grp_sap_starting_type.has_value()) {
        write_uint(out, seq, "maxGrpSapStartingType", *track.max_grp_sap_starting_type);
    }
    if (track.max_obj_sap_starting_type.has_value()) {
        write_uint(out, seq, "maxObjSapStartingType", *track.max_obj_sap_starting_type);
    }
    // Section 5: producer-defined fields, emitted last. Values are raw JSON.
    for (const auto& [key, value] : track.custom_fields) {
        write_raw(out, seq, key, value);
    }

    out << '}';
}

void write_track_array(std::ostringstream& out,
                       JsonSeq& seq,
                       std::string_view key,
                       const std::vector<MsfTrack>& tracks) {
    seq.separate();
    out << '"' << json_escape(key) << "\":[";
    JsonSeq inner(out);
    for (const auto& track : tracks) {
        inner.separate();
        write_track(out, track);
    }
    out << ']';
}

}  // namespace

std::string serialize_catalog(const MsfCatalog& catalog) {
    validate_catalog(catalog);

    std::ostringstream out;
    out << '{';
    JsonSeq seq(out);

    write_string(out, seq, "version", catalog.version);
    if (catalog.generated_at_ms.has_value()) {
        write_uint(out, seq, "generatedAt", *catalog.generated_at_ms);
    }
    // Section 5.1.3: this field MUST NOT be included if it is FALSE.
    if (catalog.is_complete.has_value() && *catalog.is_complete) {
        write_bool(out, seq, "isComplete", true);
    }

    write_track_array(out, seq, "tracks", catalog.tracks);

    if (!catalog.publish_tracks.empty()) {
        write_track_array(out, seq, "publishTracks", catalog.publish_tracks);
    }

    // Section 5.1.7: initDataList MUST be located after the tracks array.
    if (!catalog.init_data_list.empty()) {
        seq.separate();
        out << "\"initDataList\":[";
        JsonSeq inner(out);
        for (const auto& entry : catalog.init_data_list) {
            inner.separate();
            out << '{';
            JsonSeq entry_seq(out);
            write_string(out, entry_seq, "id", entry.id);
            write_string(out, entry_seq, "type", entry.type);
            write_string(out, entry_seq, "data", entry.data);
            out << '}';
        }
        out << ']';
    }

    out << '}';
    return out.str();
}

}  // namespace openmoq::publisher
