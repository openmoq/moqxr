#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/msf_catalog.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace openmoq::publisher {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string object_filename(const CmsfObject& object) {
    if (object.kind == CmsfObjectKind::kInitialization && object.track_name == "catalog") {
        return "catalog.json";
    }
    if (object.kind == CmsfObjectKind::kMetadata) {
        return object.track_name + "_g" + std::to_string(object.group_id) + "_o" + std::to_string(object.object_id) +
               ".json";
    }

    return object.track_name + "_g" + std::to_string(object.group_id) + "_o" + std::to_string(object.object_id) +
           "_media.mp4";
}

std::string init_filename(std::string_view track_name) {
    return std::string(track_name) + "_init.mp4";
}

std::string probe_filename(const CmsfObject& object) {
    return object.track_name + "_g" + std::to_string(object.group_id) + "_o" + std::to_string(object.object_id) +
           "_probe.mp4";
}

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> payload) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to create " + path.string());
    }
    output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_ascii(std::vector<std::uint8_t>& out, std::string_view value) {
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> make_box(std::string_view type, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    append_ascii(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const std::uint32_t b0 = bytes[offset];
        const std::uint32_t b1 = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
        const std::uint32_t b2 = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
        const std::uint32_t word = (b0 << 16U) | (b1 << 8U) | b2;
        encoded.push_back(kBase64Alphabet[(word >> 18U) & 0x3FU]);
        encoded.push_back(kBase64Alphabet[(word >> 12U) & 0x3FU]);
        encoded.push_back(offset + 1 < bytes.size() ? kBase64Alphabet[(word >> 6U) & 0x3FU] : '=');
        encoded.push_back(offset + 2 < bytes.size() ? kBase64Alphabet[word & 0x3FU] : '=');
    }
    return encoded;
}

std::string object_kind_name(CmsfObjectKind kind, std::string_view track_name) {
    switch (kind) {
        case CmsfObjectKind::kInitialization:
            return track_name == "catalog" ? "catalog" : "init";
        case CmsfObjectKind::kMetadata:
            return "metadata";
        case CmsfObjectKind::kMedia:
            return "media";
    }
    return "unknown";
}

std::string sap_track_name(std::string_view media_track_name) {
    return std::string(media_track_name) + "_sap";
}

std::string msf_timeline_track_name() {
    return "timeline";
}

std::uint32_t next_synthetic_track_id(const std::vector<TrackDescription>& tracks) {
    std::uint32_t next_id = 1;
    for (const auto& track : tracks) {
        next_id = std::max(next_id, track.track_id + 1);
    }
    return next_id;
}

const TrackDescription* find_track_by_name(const std::vector<TrackDescription>& tracks, std::string_view track_name) {
    const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const TrackDescription& track) {
        return track.track_name == track_name;
    });
    return it == tracks.end() ? nullptr : &*it;
}

std::uint64_t round_us_to_ms(std::uint64_t value_us) {
    return (value_us + 500ULL) / 1000ULL;
}

std::vector<std::uint8_t> build_sap_timeline_payload(const SegmentedMp4& segmented_mp4,
                                                     const TrackDescription& media_track) {
    std::ostringstream payload;
    payload << '[';
    bool first = true;
    for (const auto& fragment : segmented_mp4.fragments) {
        if (fragment.track_name != media_track.track_name) {
            continue;
        }
        if (!first) {
            payload << ',';
        }
        first = false;
        payload << '{'
                << "\"l\":[" << fragment.group_id << "," << fragment.object_id << "],"
                << "\"data\":[" << static_cast<unsigned int>(fragment.sap_type) << ','
                << round_us_to_ms(fragment.earliest_presentation_time_us) << ']'
                << '}';
    }
    payload << ']';
    const std::string text = payload.str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> build_msf_timeline_payload(const SegmentedMp4& segmented_mp4) {
    struct TimelineKey {
        std::uint64_t media_time_ms = 0;
        std::size_t group_id = 0;
        std::size_t object_id = 0;

        bool operator<(const TimelineKey& other) const {
            return std::tie(media_time_ms, group_id, object_id) <
                   std::tie(other.media_time_ms, other.group_id, other.object_id);
        }
    };

    std::set<TimelineKey> emitted;
    for (const auto& fragment : segmented_mp4.fragments) {
        emitted.insert(TimelineKey{
            .media_time_ms = round_us_to_ms(fragment.start_time_us),
            .group_id = fragment.group_id,
            .object_id = fragment.object_id,
        });
    }

    std::ostringstream payload;
    payload << '[';
    bool first = true;
    for (const auto& key : emitted) {
        if (!first) {
            payload << ',';
        }
        first = false;
        payload << '[' << key.media_time_ms << ",[" << key.group_id << ',' << key.object_id << "],0]";
    }
    payload << ']';
    const std::string text = payload.str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::uint32_t track_id_from_trak(const Mp4Box& trak, std::span<const std::uint8_t> bytes) {
    const Mp4Box* tkhd = find_child_box(trak, "tkhd");
    if (tkhd == nullptr || tkhd->payload.size < 20) {
        return 0;
    }
    const std::uint8_t version = bytes[tkhd->payload.offset];
    const std::size_t track_id_offset = tkhd->payload.offset + (version == 1 ? 20 : 12);
    if (track_id_offset + 4 > bytes.size()) {
        return 0;
    }
    return read_be32(bytes, track_id_offset);
}

std::uint32_t track_id_from_trex(const Mp4Box& trex, std::span<const std::uint8_t> bytes) {
    if (trex.payload.offset + 8 > bytes.size()) {
        return 0;
    }
    return read_be32(bytes, trex.payload.offset + 4);
}

std::vector<std::uint8_t> build_track_specific_init_segment(std::span<const std::uint8_t> init_bytes,
                                                            const TrackDescription& track,
                                                            std::size_t track_index) {
    const std::vector<Mp4Box> top_level_boxes = parse_mp4_boxes(init_bytes);
    const Mp4Box* ftyp = find_first_box(top_level_boxes, "ftyp");
    const Mp4Box* moov = find_first_box(top_level_boxes, "moov");
    if (ftyp == nullptr || moov == nullptr) {
        throw std::runtime_error("catalog generation requires ftyp and moov boxes");
    }

    std::vector<std::uint8_t> moov_payload;
    bool found_track = false;
    std::uint32_t selected_track_id = track.track_id;
    std::size_t trak_index = 0;

    for (const auto& child : moov->children) {
        if (child.type == "trak") {
            const std::uint32_t child_track_id = track_id_from_trak(child, init_bytes);
            const bool matches = track.track_id != 0 ? child_track_id == track.track_id : trak_index == track_index;
            if (matches) {
                const auto child_bytes = slice_bytes(init_bytes, child.span);
                moov_payload.insert(moov_payload.end(), child_bytes.begin(), child_bytes.end());
                found_track = true;
                selected_track_id = child_track_id;
            }
            ++trak_index;
            continue;
        }

        if (child.type == "mvex") {
            std::vector<std::uint8_t> mvex_payload;
            std::size_t trex_index = 0;
            for (const auto& mvex_child : child.children) {
                if (mvex_child.type == "trex") {
                    const std::uint32_t trex_track_id = track_id_from_trex(mvex_child, init_bytes);
                    const bool matches =
                        selected_track_id != 0 ? trex_track_id == selected_track_id : trex_index == track_index;
                    ++trex_index;
                    if (!matches) {
                        continue;
                    }
                }
                const auto mvex_child_bytes = slice_bytes(init_bytes, mvex_child.span);
                mvex_payload.insert(mvex_payload.end(), mvex_child_bytes.begin(), mvex_child_bytes.end());
            }
            if (!mvex_payload.empty()) {
                const auto mvex_box = make_box("mvex", mvex_payload);
                moov_payload.insert(moov_payload.end(), mvex_box.begin(), mvex_box.end());
            }
            continue;
        }

        const auto child_bytes = slice_bytes(init_bytes, child.span);
        moov_payload.insert(moov_payload.end(), child_bytes.begin(), child_bytes.end());
    }

    if (!found_track) {
        throw std::runtime_error("catalog generation could not locate track-specific initialization data");
    }

    const auto moov_box = make_box("moov", moov_payload);
    const auto ftyp_bytes = slice_bytes(init_bytes, ftyp->span);

    std::vector<std::uint8_t> track_init;
    track_init.insert(track_init.end(), ftyp_bytes.begin(), ftyp_bytes.end());
    track_init.insert(track_init.end(), moov_box.begin(), moov_box.end());
    return track_init;
}

std::vector<std::uint8_t> extract_codec_init_data(const Mp4Box& sample_entry,
                                                  std::span<const std::uint8_t> bytes,
                                                  std::size_t child_offset) {
    for (std::size_t scan_offset = child_offset; scan_offset + 8 <= sample_entry.span.size; ++scan_offset) {
        const std::size_t cursor = sample_entry.span.offset + scan_offset;
        const std::uint32_t box_size = read_be32(bytes, cursor);
        if (box_size < 8 || cursor + box_size > sample_entry.span.offset + sample_entry.span.size) {
            continue;
        }

        const std::string type(reinterpret_cast<const char*>(bytes.data() + cursor + 4), 4);
        if (type == "avcC" || type == "hvcC" || type == "esds" || type == "dOps") {
            return std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                             bytes.begin() + static_cast<std::ptrdiff_t>(cursor + box_size));
        }
    }

    throw std::runtime_error("catalog generation could not locate codec initData box in sample entry");
}

std::vector<std::uint8_t> build_track_codec_init_data(std::span<const std::uint8_t> init_bytes,
                                                      const TrackDescription& track,
                                                      std::size_t track_index) {
    const std::vector<Mp4Box> top_level_boxes = parse_mp4_boxes(init_bytes);
    const Mp4Box* moov = find_first_box(top_level_boxes, "moov");
    if (moov == nullptr) {
        throw std::runtime_error("catalog generation requires moov box");
    }

    std::size_t trak_index = 0;
    for (const auto& child : moov->children) {
        if (child.type != "trak") {
            continue;
        }

        const std::uint32_t child_track_id = track_id_from_trak(child, init_bytes);
        const bool matches = track.track_id != 0 ? child_track_id == track.track_id : trak_index == track_index;
        ++trak_index;
        if (!matches) {
            continue;
        }

        const Mp4Box* mdia = find_child_box(child, "mdia");
        const Mp4Box* minf = mdia == nullptr ? nullptr : find_child_box(*mdia, "minf");
        const Mp4Box* stbl = minf == nullptr ? nullptr : find_child_box(*minf, "stbl");
        const Mp4Box* stsd = stbl == nullptr ? nullptr : find_child_box(*stbl, "stsd");
        if (stsd == nullptr || stsd->payload.size < 16) {
            throw std::runtime_error("catalog generation requires stsd for codec initData");
        }

        const std::size_t sample_entry_offset = stsd->payload.offset + 8;
        const std::uint32_t sample_entry_size = read_be32(init_bytes, sample_entry_offset);
        if (sample_entry_size < 8 || sample_entry_offset + sample_entry_size > init_bytes.size()) {
            throw std::runtime_error("catalog generation found invalid stsd sample entry");
        }

        const Mp4Box sample_entry{
            .type = std::string(reinterpret_cast<const char*>(init_bytes.data() + sample_entry_offset + 4), 4),
            .span = {.offset = sample_entry_offset, .size = sample_entry_size},
            .payload = {.offset = sample_entry_offset + 8, .size = sample_entry_size - 8},
            .children = {},
        };

        if (track.handler_type == "vide") {
            return extract_codec_init_data(sample_entry, init_bytes, 8 + 70);
        }
        if (track.handler_type == "soun") {
            return extract_codec_init_data(sample_entry, init_bytes, 8 + 28);
        }

        throw std::runtime_error("catalog generation does not support codec initData for handler " + track.handler_type);
    }

    throw std::runtime_error("catalog generation could not locate matching trak for codec initData");
}

}  // namespace

PublishPlan build_publish_plan(const SegmentedMp4& segmented_mp4,
                               DraftVersion version,
                               bool include_sap,
                               bool include_msf_timeline) {
    std::vector<TrackDescription> tracks = segmented_mp4.tracks;
    std::uint32_t synthetic_track_id = next_synthetic_track_id(tracks);
    if (include_msf_timeline) {
        std::vector<std::string> media_track_names;
        media_track_names.reserve(segmented_mp4.tracks.size());
        for (const auto& media_track : segmented_mp4.tracks) {
            media_track_names.push_back(media_track.track_name);
        }
        tracks.push_back(TrackDescription{
            .track_id = synthetic_track_id++,
            .handler_type = "meta",
            .codec = {},
            .sample_entry_type = "mediatimeline",
            .track_name = msf_timeline_track_name(),
            .packaging = "mediatimeline",
            .event_type = {},
            .mime_type = "application/json",
            .depends = std::move(media_track_names),
        });
    }
    if (include_sap) {
        for (const auto& media_track : segmented_mp4.tracks) {
            tracks.push_back(TrackDescription{
                .track_id = synthetic_track_id++,
                .handler_type = "meta",
                .codec = {},
                .sample_entry_type = "eventtimeline",
                .track_name = sap_track_name(media_track.track_name),
                .packaging = "eventtimeline",
                .event_type = "org.ietf.moq.cmsf.sap",
                .mime_type = "application/json",
                .depends = {media_track.track_name},
            });
        }
    }
    tracks.insert(tracks.begin(), TrackDescription{
                                    .track_id = 0,
                                    .handler_type = "meta",
                                    .codec = "catalog",
                                    .sample_entry_type = "catalog",
                                    .track_name = "catalog",
                                    .packaging = "catalog",
                                    .event_type = {},
                                    .mime_type = {},
                                    .depends = {},
                                });

    PublishPlan plan{
        .draft = draft_profile(version),
        .tracks = std::move(tracks),
        .track_initializations = {},
        .objects = {},
    };

    std::map<std::string, std::string> init_data_by_track;
    for (std::size_t index = 0; index < segmented_mp4.tracks.size(); ++index) {
        const auto& track = segmented_mp4.tracks[index];
        const std::vector<std::uint8_t> codec_init_data =
            build_track_codec_init_data(segmented_mp4.initialization_segment.owned_bytes, track, index);
        const std::vector<std::uint8_t> init_segment =
            build_track_specific_init_segment(segmented_mp4.initialization_segment.owned_bytes, track, index);
        plan.track_initializations.push_back({
            .track_name = track.track_name,
            .codec_payload = codec_init_data,
            .init_segment = init_segment,
        });
        init_data_by_track.emplace(track.track_name, base64_encode(init_segment));
    }

    MsfCatalog msf_catalog;
    // Section 5.1.2: generatedAt SHOULD NOT be included when isLive is false,
    // which is always the case for a batch publish plan.
    for (const auto& track : plan.tracks) {
        if (track.track_name == "catalog") {
            continue;
        }
        MsfTrack msf_track = make_msf_track(track, /*is_live=*/false);

        // CMSF section 3.5.2: maxObjSapStartingType is the maximum SAP type
        // across every fragment of the track; maxGrpSapStartingType is the
        // maximum across only the first Object of each Group (object_id ==
        // 0), so it is always <= maxObjSapStartingType. Both are Optional --
        // leave them unset when the track has no SAP information at all.
        bool has_sap_info = false;
        std::uint32_t max_obj_sap_starting_type = 0;
        std::uint32_t max_grp_sap_starting_type = 0;
        for (const auto& fragment : segmented_mp4.fragments) {
            if (fragment.track_name != track.track_name || !fragment.has_sap_type) {
                continue;
            }
            has_sap_info = true;
            max_obj_sap_starting_type = std::max(max_obj_sap_starting_type, static_cast<std::uint32_t>(fragment.sap_type));
            if (fragment.object_id == 0) {
                max_grp_sap_starting_type = std::max(max_grp_sap_starting_type, static_cast<std::uint32_t>(fragment.sap_type));
            }
        }
        if (has_sap_info) {
            msf_track.max_grp_sap_starting_type = max_grp_sap_starting_type;
            msf_track.max_obj_sap_starting_type = max_obj_sap_starting_type;
        }

        const auto init_it = init_data_by_track.find(track.track_name);
        if (init_it != init_data_by_track.end()) {
            attach_init_data(msf_catalog, msf_track, track.track_name, init_it->second);
        }

        msf_catalog.tracks.push_back(std::move(msf_track));
    }

    const std::string catalog_text = serialize_catalog(msf_catalog);
    const std::vector<std::uint8_t> catalog_payload(catalog_text.begin(), catalog_text.end());
    plan.objects.push_back({
        .kind = CmsfObjectKind::kInitialization,
        .track_name = "catalog",
        .group_id = 0,
        .object_id = 0,
        .payload = {},
        .owned_payload = catalog_payload,
    });

    for (const auto& fragment : segmented_mp4.fragments) {
        plan.objects.push_back({
            .kind = CmsfObjectKind::kMedia,
            .track_name = fragment.track_name,
            .group_id = fragment.group_id,
            .object_id = fragment.object_id,
            .media_time_us = fragment.start_time_us,
            .media_duration_us = fragment.duration_us,
            .sap_type = fragment.sap_type,         // carry CMSF SAP type into the object
            .has_sap_type = fragment.has_sap_type,
            .payload = fragment.payload.span,
            .owned_payload = fragment.payload.owned_bytes,
        });
    }

    if (include_msf_timeline) {
        plan.objects.push_back({
            .kind = CmsfObjectKind::kMetadata,
            .track_name = msf_timeline_track_name(),
            .group_id = 0,
            .object_id = 0,
            .payload = {},
            .owned_payload = build_msf_timeline_payload(segmented_mp4),
        });
    }

    if (include_sap) {
        for (const auto& media_track : segmented_mp4.tracks) {
            if (find_track_by_name(plan.tracks, sap_track_name(media_track.track_name)) == nullptr) {
                continue;
            }
            plan.objects.push_back({
                .kind = CmsfObjectKind::kMetadata,
                .track_name = sap_track_name(media_track.track_name),
                .group_id = 0,
                .object_id = 0,
                .payload = {},
                .owned_payload = build_sap_timeline_payload(segmented_mp4, media_track),
            });
        }
    }

    return plan;
}

std::string render_publish_plan(const PublishPlan& plan) {
    std::ostringstream stream;
    stream << "draft=" << to_string(plan.draft.version) << '\n';
    stream << "notes=" << plan.draft.notes << '\n';
    stream << "tracks=" << summarize_tracks(plan.tracks) << '\n';
    stream << "objects=" << plan.objects.size() << '\n';

    for (const auto& object : plan.objects) {
        stream << object.track_name << " group=" << object.group_id << " object=" << object.object_id
               << " bytes=" << (object.owned_payload.empty() ? object.payload.size : object.owned_payload.size())
               << " time_us=" << object.media_time_us
               << " kind="
               << object_kind_name(object.kind, object.track_name) << '\n';
    }

    return stream.str();
}

PublishPlan materialize_publish_plan(const PublishPlan& plan, std::span<const std::uint8_t> bytes) {
    PublishPlan materialized = plan;

    for (auto& object : materialized.objects) {
        if (!object.owned_payload.empty() || object.payload.size == 0) {
            continue;
        }

        const auto payload = slice_bytes(bytes, object.payload);
        object.owned_payload.assign(payload.begin(), payload.end());
        object.payload = {};
    }

    return materialized;
}

void emit_plan_objects(const PublishPlan& plan,
                       std::span<const std::uint8_t> bytes,
                       const std::filesystem::path& output_dir) {
    std::filesystem::create_directories(output_dir);

    for (const auto& track_init : plan.track_initializations) {
        write_bytes(output_dir / init_filename(track_init.track_name), track_init.init_segment);
    }

    for (const auto& object : plan.objects) {
        if (object.owned_payload.empty()) {
            write_bytes(output_dir / object_filename(object), slice_bytes(bytes, object.payload));
        } else {
            write_bytes(output_dir / object_filename(object), object.owned_payload);
        }

        if (object.kind != CmsfObjectKind::kMedia) {
            continue;
        }

        const auto track_init_it = std::find_if(plan.track_initializations.begin(),
                                                plan.track_initializations.end(),
                                                [&](const TrackInitialization& init) {
                                                    return init.track_name == object.track_name;
                                                });
        if (track_init_it == plan.track_initializations.end()) {
            continue;
        }

        std::vector<std::uint8_t> probe_payload = track_init_it->init_segment;
        const auto media_payload =
            object.owned_payload.empty() ? slice_bytes(bytes, object.payload) : std::span<const std::uint8_t>(object.owned_payload);
        probe_payload.insert(probe_payload.end(), media_payload.begin(), media_payload.end());
        write_bytes(output_dir / probe_filename(object), probe_payload);
    }

    std::ofstream manifest(output_dir / "publish-plan.txt");
    if (!manifest) {
        throw std::runtime_error("failed to create publish-plan.txt");
    }
    manifest << render_publish_plan(plan);
}

LiveCatalog build_live_catalog(const std::vector<TrackDescription>& tracks,
                               std::span<const std::uint8_t> init_segment,
                               bool is_live) {
    LiveCatalog result;

    // Build per-track init segments (single-track moov), matching the static file publish path.
    // Each track's initData must be a CMAF init segment containing only that track's trak/trex.
    std::map<std::string, std::string> init_data_by_track;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const auto& track = tracks[index];
        std::vector<std::uint8_t> track_init = build_track_specific_init_segment(init_segment, track, index);
        init_data_by_track.emplace(track.track_name, base64_encode(track_init));
        result.track_initializations.push_back({
            .track_name = track.track_name,
            .codec_payload = {},
            .init_segment = std::move(track_init),
        });
    }

    MsfCatalog msf_catalog;
    if (is_live) {
        // Section 5.1.2: SHOULD NOT be included when isLive is false.
        msf_catalog.generated_at_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    for (const auto& track : tracks) {
        MsfTrack msf_track = make_msf_track(track, is_live);

        const auto init_it = init_data_by_track.find(track.track_name);
        if (init_it != init_data_by_track.end()) {
            attach_init_data(msf_catalog, msf_track, track.track_name, init_it->second);
        }

        msf_catalog.tracks.push_back(std::move(msf_track));
    }

    const std::string catalog_text = serialize_catalog(msf_catalog);
    result.catalog_payload = std::vector<std::uint8_t>(catalog_text.begin(), catalog_text.end());
    return result;
}

std::string track_init_data_base64(std::span<const std::uint8_t> init_segment,
                                   const TrackDescription& track,
                                   std::size_t track_index) {
    return base64_encode(build_track_specific_init_segment(init_segment, track, track_index));
}

}  // namespace openmoq::publisher
