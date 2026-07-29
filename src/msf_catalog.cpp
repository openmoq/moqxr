#include "openmoq/publisher/msf_catalog.h"

#include <cmath>
#include <iomanip>
#include <ios>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
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
    // Section 5.3: a delta update MUST include at least one operation and
    // MUST NOT carry tracks. Only "add" and "remove" are emitted by this
    // implementation.
    if (!catalog.delta_update.empty()) {
        if (!catalog.tracks.empty()) {
            throw std::runtime_error("MSF delta catalog must not carry a tracks array");
        }
        for (const auto& op : catalog.delta_update) {
            if (op.op != "add" && op.op != "remove") {
                throw std::runtime_error("MSF delta catalog has unsupported operation \"" + op.op + "\"");
            }
            if (op.tracks.empty()) {
                throw std::runtime_error("MSF delta operation \"" + op.op + "\" has no tracks");
            }
        }
        return;
    }

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

    const bool is_delta = !catalog.delta_update.empty();

    std::ostringstream out;
    out << '{';
    JsonSeq seq(out);

    // Section 5.3: a delta update MUST NOT contain a Tracks field or an MSF
    // version field.
    if (!is_delta) {
        write_string(out, seq, "version", catalog.version);
    }
    if (catalog.generated_at_ms.has_value()) {
        write_uint(out, seq, "generatedAt", *catalog.generated_at_ms);
    }
    if (catalog.is_complete.has_value() && *catalog.is_complete) {
        write_bool(out, seq, "isComplete", true);
    }

    if (is_delta) {
        seq.separate();
        out << "\"deltaUpdate\":[";
        JsonSeq ops(out);
        for (const auto& op : catalog.delta_update) {
            ops.separate();
            out << "{\"op\":\"" << json_escape(op.op) << "\",\"tracks\":[";
            JsonSeq op_tracks(out);
            for (const auto& track : op.tracks) {
                op_tracks.separate();
                if (op.op == "remove") {
                    // Section 5.1.6: a remove entry MUST include the track
                    // name, MAY include the namespace, and MUST NOT hold any
                    // other field. The differ builds remove operations from
                    // the previously published tracks, which are complete, so
                    // narrowing to name and namespace HERE is deliberate and
                    // required -- it is not a lossy accident. Validating the
                    // input instead would reject the differ's own output.
                    out << "{\"name\":\"" << json_escape(track.name) << '"';
                    if (track.name_space.has_value()) {
                        out << ",\"namespace\":\"" << json_escape(*track.name_space) << '"';
                    }
                    out << '}';
                } else {
                    write_track(out, track);
                }
            }
            out << "]}";
        }
        out << ']';
    } else {
        write_track_array(out, seq, "tracks", catalog.tracks);
    }

    if (!is_delta && !catalog.publish_tracks.empty()) {
        write_track_array(out, seq, "publishTracks", catalog.publish_tracks);
    }

    // Section 5.1.7: initDataList MUST be located after the tracks array.
    if (!is_delta && !catalog.init_data_list.empty()) {
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

namespace {

constexpr std::uint64_t kDefaultVideoBitrate = 2000000;
constexpr std::uint64_t kDefaultAudioBitrate = 128000;

std::uint64_t codec_class_default(const TrackDescription& track) {
    return track.handler_type == "soun" ? kDefaultAudioBitrate : kDefaultVideoBitrate;
}

}  // namespace

namespace {

// resolve_bitrate's codec-class-default rung keeps the catalog conformant
// (MSF 5.2.22 requires bitrate) when no btrt box and no measurement is
// available, but the resulting figure is fabricated. Warn the operator so
// the estimate is visible rather than silently published as fact.
void warn_if_bitrate_estimated(const TrackDescription& track,
                               std::optional<std::uint64_t> configured,
                               std::uint64_t computed,
                               std::uint64_t resolved) {
    if (bitrate_is_estimated(track, configured, computed)) {
        std::cerr << "[msf-catalog] warning: track \"" << track.track_name
                  << "\" has no btrt and no configured bitrate; advertising estimated "
                  << resolved << " bps" << std::endl;
    }
}

}  // namespace

std::uint64_t resolve_bitrate(const TrackDescription& track,
                              std::optional<std::uint64_t> configured,
                              std::uint64_t computed) {
    if (track.max_bitrate != 0) {
        return track.max_bitrate;
    }
    if (configured.has_value() && *configured != 0) {
        return *configured;
    }
    if (computed != 0) {
        return computed;
    }
    return codec_class_default(track);
}

bool bitrate_is_estimated(const TrackDescription& track,
                          std::optional<std::uint64_t> configured,
                          std::uint64_t computed) {
    return track.max_bitrate == 0 && !(configured.has_value() && *configured != 0) && computed == 0;
}

MsfTrack make_msf_track(const TrackDescription& track,
                        bool is_live,
                        std::optional<std::uint64_t> configured_bitrate,
                        std::uint64_t computed_bitrate) {
    MsfTrack out;
    out.name = track.track_name;
    out.packaging = track.packaging;
    out.is_live = is_live;

    if (track.handler_type == "vide") {
        out.role = "video";
    } else if (track.handler_type == "soun") {
        out.role = "audio";
    } else if (track.packaging == "mediatimeline") {
        out.role = "mediatimeline";
    } else if (track.packaging == "eventtimeline") {
        out.role = "eventtimeline";
    }

    if (!track.codec.empty()) {
        out.codec = track.codec;
    }
    if (!track.mime_type.empty()) {
        out.mime_type = track.mime_type;
    }
    if (!track.event_type.empty()) {
        out.event_type = track.event_type;
    }
    if (!track.depends.empty()) {
        out.depends = track.depends;
    }
    if (!track.language.empty()) {
        out.lang = track.language;
    }
    if (track.timescale != 0) {
        out.timescale = track.timescale;
    }
    // Section 5.2.35 forbids trackDuration on a live track.
    if (!is_live && track.duration_ms != 0) {
        out.track_duration_ms = track.duration_ms;
    }

    if (track.handler_type == "vide") {
        out.render_group = 1;
        // Sections 5.2.26/5.2.27: width/height are optional ("SHOULD accompany
        // tracks which have a visual representation"), so 0 (unknown, e.g. an
        // av01/vp09 sample entry extract_tracks does not yet decode dimensions
        // for) must be omitted rather than published as a literal 0x0. This is
        // deliberately asymmetric with samplerate/channelConfig below, which
        // validate_track makes MUST-present for audio and so must stay
        // unconditional even when the value is unknown.
        if (track.width != 0) {
            out.width = track.width;
        }
        if (track.height != 0) {
            out.height = track.height;
        }
        if (track.frame_rate > 0.0) {
            out.framerate = track.frame_rate;
        }
        const std::uint64_t bitrate = resolve_bitrate(track, configured_bitrate, computed_bitrate);
        out.bitrate = bitrate;
        warn_if_bitrate_estimated(track, configured_bitrate, computed_bitrate, bitrate);
    } else if (track.handler_type == "soun") {
        out.render_group = 1;
        out.samplerate = track.sample_rate;
        out.channel_config = std::to_string(track.channel_count);
        const std::uint64_t bitrate = resolve_bitrate(track, configured_bitrate, computed_bitrate);
        out.bitrate = bitrate;
        warn_if_bitrate_estimated(track, configured_bitrate, computed_bitrate, bitrate);
    }

    if (track.avg_bitrate != 0) {
        out.avg_bitrate = track.avg_bitrate;
    }

    return out;
}

void attach_init_data(MsfCatalog& catalog, MsfTrack& track, std::string_view track_name, std::string base64_data) {
    std::string init_id(track_name);
    init_id += "-init";
    track.init_ref = init_id;
    catalog.init_data_list.push_back(MsfInitData{
        .id = std::move(init_id),
        .type = "inline",
        .data = std::move(base64_data),
    });
}

MsfCatalog make_end_broadcast_catalog(const MsfCatalog& current,
                                      EndBroadcastMode mode,
                                      const std::map<std::string, std::uint64_t>& track_durations_ms) {
    MsfCatalog out;
    out.version = current.version;

    if (mode == EndBroadcastMode::kTerminate) {
        // Section 11.3: signal isComplete TRUE with an empty Tracks field.
        // Section 5.1.3: isComplete MUST NOT be removed once added.
        out.is_complete = true;
        return out;
    }

    // kConvertToVod. Section 5.1.2: generatedAt SHOULD NOT be included when
    // isLive is false, so it is deliberately not copied.
    out.init_data_list = current.init_data_list;
    out.tracks.reserve(current.tracks.size());
    for (MsfTrack track : current.tracks) {
        track.is_live = false;
        const auto duration_it = track_durations_ms.find(track.name);
        if (duration_it != track_durations_ms.end()) {
            track.track_duration_ms = duration_it->second;
        }
        out.tracks.push_back(std::move(track));
    }
    // publishTracks carry subscriber-uploaded logs and metrics (MSF sections 9–10),
    // not media. They get is_live cleared but never receive trackDuration, because
    // Section 11.3 mandates duration only for the media Tracks field.
    for (MsfTrack track : current.publish_tracks) {
        track.is_live = false;
        out.publish_tracks.push_back(std::move(track));
    }
    return out;
}

namespace {

bool tracks_equal(const MsfTrack& a, const MsfTrack& b) {
    // Compare every field that serialize_catalog can emit. A field added to
    // MsfTrack without being added here would make a real change look like a
    // no-op, so keep this in step with write_track.
    return a.name == b.name && a.name_space == b.name_space && a.packaging == b.packaging &&
           a.role == b.role && a.is_live == b.is_live && a.target_latency_ms == b.target_latency_ms &&
           a.label == b.label && a.render_group == b.render_group && a.alt_group == b.alt_group &&
           a.init_ref == b.init_ref && a.depends == b.depends && a.codec == b.codec &&
           a.mime_type == b.mime_type && a.framerate == b.framerate && a.timescale == b.timescale &&
           a.bitrate == b.bitrate && a.avg_bitrate == b.avg_bitrate &&
           a.max_gop_duration_ms == b.max_gop_duration_ms &&
           a.max_group_duration_ms == b.max_group_duration_ms && a.width == b.width &&
           a.height == b.height && a.samplerate == b.samplerate &&
           a.channel_config == b.channel_config && a.lang == b.lang &&
           a.track_duration_ms == b.track_duration_ms && a.event_type == b.event_type &&
           a.max_grp_sap_starting_type == b.max_grp_sap_starting_type &&
           a.max_obj_sap_starting_type == b.max_obj_sap_starting_type &&
           a.custom_fields == b.custom_fields &&
           ((!a.buffers.has_value() && !b.buffers.has_value()) ||
            (a.buffers.has_value() && b.buffers.has_value() &&
             a.buffers->target_ms == b.buffers->target_ms && a.buffers->min_ms == b.buffers->min_ms &&
             a.buffers->max_ms == b.buffers->max_ms));
}

bool catalogs_equal(const MsfCatalog& a, const MsfCatalog& b) {
    if (a.version != b.version || a.is_complete != b.is_complete ||
        a.tracks.size() != b.tracks.size() || a.publish_tracks.size() != b.publish_tracks.size() ||
        a.init_data_list.size() != b.init_data_list.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        if (!tracks_equal(a.tracks[i], b.tracks[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.publish_tracks.size(); ++i) {
        if (!tracks_equal(a.publish_tracks[i], b.publish_tracks[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.init_data_list.size(); ++i) {
        if (a.init_data_list[i].id != b.init_data_list[i].id ||
            a.init_data_list[i].type != b.init_data_list[i].type ||
            a.init_data_list[i].data != b.init_data_list[i].data) {
            return false;
        }
    }
    // generated_at_ms is deliberately excluded: a fresh timestamp on an
    // otherwise identical catalog is not a change worth republishing.
    return true;
}

}  // namespace

CatalogObject CatalogPublisher::emit_independent(const MsfCatalog& catalog) {
    // serialize_catalog can throw (validate_catalog, and per-track checks
    // inside write_track -- validate_catalog does not call validate_track).
    // Serialize into a local first and only touch the counters once it has
    // succeeded: assigning next_group_id_++ before serializing would leave
    // the counter advanced past a group whose object 0 was never actually
    // sent on a throw, and a later delta would land in that phantom group,
    // violating MSF section 5.
    std::string payload = serialize_catalog(catalog);

    CatalogObject object;
    object.group_id = next_group_id_++;
    object.object_id = 0;
    object.subgroup_id = 0;
    object.payload = std::move(payload);
    next_object_id_ = 1;
    deltas_in_group_ = 0;
    return object;
}

bool CatalogPublisher::ended() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_;
}

void CatalogPublisher::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_.reset();
    next_group_id_ = 0;
    next_object_id_ = 0;
    ended_ = false;
    deltas_in_group_ = 0;
}

std::vector<CatalogObject> CatalogPublisher::publish(const MsfCatalog& desired) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ended_) {
        throw std::runtime_error("MSF catalog publish after end_broadcast");
    }
    if (!last_.has_value()) {
        std::vector<CatalogObject> out;
        out.push_back(emit_independent(desired));
        last_ = desired;
        return out;
    }
    if (catalogs_equal(*last_, desired)) {
        return {};
    }

    // Section 5.3's freeze rule and the differ below only reason about
    // `desired.tracks`; they never run `desired` through the catalog-wide
    // checks (duplicate names, dangling initRef, ...) the way the
    // full-independent path does via serialize_catalog. Validate explicitly
    // so a malformed `desired` still throws instead of silently producing a
    // delta from an invalid catalog.
    validate_catalog(desired);

    // Key tracks by the namespace and name tuple, which section 5.3 treats as
    // the identity of a track.
    const auto key_of = [](const MsfTrack& track) {
        return std::pair<std::string, std::string>(track.name_space.value_or(std::string{}), track.name);
    };
    std::map<std::pair<std::string, std::string>, const MsfTrack*> before;
    for (const auto& track : last_->tracks) {
        before.emplace(key_of(track), &track);
    }
    std::map<std::pair<std::string, std::string>, const MsfTrack*> after;
    for (const auto& track : desired.tracks) {
        after.emplace(key_of(track), &track);
    }

    std::vector<MsfTrack> added;
    std::vector<MsfTrack> removed;
    for (const auto& [key, track] : after) {
        const auto it = before.find(key);
        if (it == before.end()) {
            added.push_back(*track);
        } else if (!tracks_equal(*it->second, *track)) {
            // Section 5.3 freezes a track's attributes once its tuple is
            // declared, so an attribute change cannot be expressed as a
            // delta. Fall back to a full independent catalog: emitting
            // nothing would leave subscribers on stale attributes.
            std::vector<CatalogObject> out;
            out.push_back(emit_independent(desired));
            last_ = desired;
            return out;
        }
    }
    for (const auto& [key, track] : before) {
        if (after.find(key) == after.end()) {
            removed.push_back(*track);
        }
    }

    // Anything other than a pure add/remove of whole tracks, including a
    // change to initDataList or publishTracks, needs a full catalog. This
    // must be a full equality check, not just a size comparison: a same-size
    // initDataList or publishTracks whose *content* differs is exactly as
    // inexpressible as a delta, and if it coincides with a track add/remove
    // in the same publish() call, the "added/removed both empty" branch
    // below would not catch it -- the content change would be silently
    // dropped even though `last_` gets updated to reflect it.
    bool init_data_changed = last_->init_data_list.size() != desired.init_data_list.size();
    for (std::size_t i = 0; !init_data_changed && i < last_->init_data_list.size(); ++i) {
        const auto& a = last_->init_data_list[i];
        const auto& b = desired.init_data_list[i];
        init_data_changed = a.id != b.id || a.type != b.type || a.data != b.data;
    }
    bool publish_tracks_changed = last_->publish_tracks.size() != desired.publish_tracks.size();
    for (std::size_t i = 0; !publish_tracks_changed && i < last_->publish_tracks.size(); ++i) {
        publish_tracks_changed = !tracks_equal(last_->publish_tracks[i], desired.publish_tracks[i]);
    }
    if ((added.empty() && removed.empty()) || init_data_changed || publish_tracks_changed ||
        deltas_in_group_ >= max_deltas_per_group_) {
        std::vector<CatalogObject> out;
        out.push_back(emit_independent(desired));
        last_ = desired;
        return out;
    }

    MsfCatalog delta;
    if (!added.empty()) {
        delta.delta_update.push_back(MsfDeltaOp{.op = "add", .tracks = std::move(added)});
    }
    if (!removed.empty()) {
        delta.delta_update.push_back(MsfDeltaOp{.op = "remove", .tracks = std::move(removed)});
    }

    // serialize_catalog can throw (write_track's per-track checks run for
    // "add" ops). Serialize into a local first: assigning next_object_id_++
    // before serializing would leave the counter advanced past an object
    // that was never actually sent on a throw, and the next successful delta
    // would then skip an object_id, violating MSF section 5's sequencing.
    std::string payload = serialize_catalog(delta);

    CatalogObject object;
    // Stay in the current group. next_group_id_ is at least 1 here: this
    // branch is only reachable when last_ has a value, which means
    // emit_independent already ran and incremented it. Do not hoist this
    // expression above that guard.
    object.group_id = next_group_id_ - 1;
    object.object_id = next_object_id_++;
    object.subgroup_id = 0;
    object.payload = std::move(payload);

    last_ = desired;
    ++deltas_in_group_;
    return {object};
}

std::vector<CatalogObject> CatalogPublisher::force_independent() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ended_) {
        throw std::runtime_error("MSF catalog force_independent after end_broadcast");
    }
    if (!last_.has_value()) {
        return {};
    }
    std::vector<CatalogObject> out;
    out.push_back(emit_independent(*last_));
    return out;
}

std::vector<CatalogObject> CatalogPublisher::end_broadcast(
    EndBroadcastMode mode,
    const std::map<std::string, std::uint64_t>& track_durations_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ended_) {
        throw std::runtime_error("MSF catalog end_broadcast called twice");
    }
    const MsfCatalog base = last_.value_or(MsfCatalog{});
    const MsfCatalog final_catalog = make_end_broadcast_catalog(base, mode, track_durations_ms);
    std::vector<CatalogObject> out;
    out.push_back(emit_independent(final_catalog));
    last_ = final_catalog;
    ended_ = true;
    return out;
}

}  // namespace openmoq::publisher
