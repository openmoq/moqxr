#include "openmoq/publisher/cmaf_segmenter.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace openmoq::publisher {

namespace {

struct ChunkMapEntry {
    std::uint32_t first_chunk = 0;
    std::uint32_t samples_per_chunk = 0;
};

struct TrackRemuxInfo {
    TrackDescription description;
    std::uint32_t timescale = 0;
    std::vector<std::uint64_t> sample_offsets;
    std::vector<std::uint32_t> sample_sizes;
    std::vector<std::uint32_t> sample_durations;
    std::vector<std::int32_t> composition_offsets;
    std::vector<bool> sync_samples;
};

struct SampleObjectInfo {
    std::uint64_t decode_time = 0;
    std::uint32_t duration = 0;
    std::uint32_t size = 0;
    std::uint32_t flags = 0;
    std::int32_t composition_offset = 0;
    ByteSpan payload_span;
};

struct FragmentTiming {
    std::uint64_t start_time_us = 0;
    std::uint64_t duration_us = 0;
    std::uint64_t earliest_presentation_time_us = 0;
    std::uint8_t sap_type = 0;
};

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t read_full_box_flags(const Mp4Box& box, std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[box.payload.offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[box.payload.offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[box.payload.offset + 3]);
}

std::uint64_t scale_to_us(std::uint64_t value, std::uint32_t timescale) {
    if (timescale == 0) {
        return 0;
    }
    return (value * 1000000ULL) / timescale;
}

std::string fragment_track_name(const Mp4Box& moof,
                                const std::vector<TrackDescription>& tracks,
                                std::span<const std::uint8_t> bytes) {
    const Mp4Box* traf = find_child_box(moof, "traf");
    const Mp4Box* tfhd = traf == nullptr ? nullptr : find_child_box(*traf, "tfhd");
    if (tfhd == nullptr || tfhd->payload.size < 8) {
        return tracks.size() == 1 ? tracks.front().track_name : "media";
    }

    const std::uint32_t track_id = read_be32(bytes, tfhd->payload.offset + 4);
    for (const auto& track : tracks) {
        if (track.track_id == track_id) {
            return track.track_name;
        }
    }
    return "media";
}

const TrackDescription* fragment_track_description(const Mp4Box& moof,
                                                   const std::vector<TrackDescription>& tracks,
                                                   std::span<const std::uint8_t> bytes) {
    const Mp4Box* traf = find_child_box(moof, "traf");
    const Mp4Box* tfhd = traf == nullptr ? nullptr : find_child_box(*traf, "tfhd");
    if (tfhd == nullptr || tfhd->payload.size < 8) {
        return tracks.size() == 1 ? &tracks.front() : nullptr;
    }

    const std::uint32_t track_id = read_be32(bytes, tfhd->payload.offset + 4);
    for (const auto& track : tracks) {
        if (track.track_id == track_id) {
            return &track;
        }
    }
    return nullptr;
}

void patch_sample_entry_type(std::vector<std::uint8_t>& init_bytes, const TrackDescription& track, std::size_t track_index) {
    const std::vector<Mp4Box> top_level_boxes = parse_mp4_boxes(init_bytes);
    const Mp4Box* moov = find_first_box(top_level_boxes, "moov");
    if (moov == nullptr) {
        return;
    }

    std::size_t trak_index = 0;
    for (const auto& child : moov->children) {
        if (child.type != "trak") {
            continue;
        }

        const Mp4Box* tkhd = find_child_box(child, "tkhd");
        std::uint32_t child_track_id = 0;
        if (tkhd != nullptr && tkhd->payload.size >= 20) {
            const std::uint8_t version = init_bytes[tkhd->payload.offset];
            const std::size_t track_id_offset = tkhd->payload.offset + (version == 1 ? 20 : 12);
            if (track_id_offset + 4 <= init_bytes.size()) {
                child_track_id = read_be32(init_bytes, track_id_offset);
            }
        }

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
            return;
        }

        const std::size_t sample_entry_type_offset = stsd->payload.offset + 12;
        if (sample_entry_type_offset + 4 > init_bytes.size() || track.sample_entry_type.size() != 4) {
            return;
        }

        std::copy(track.sample_entry_type.begin(), track.sample_entry_type.end(), init_bytes.begin() + sample_entry_type_offset);
        return;
    }
}

void patch_init_segment_sample_entries(std::vector<std::uint8_t>& init_bytes, const std::vector<TrackDescription>& tracks) {
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        patch_sample_entry_type(init_bytes, tracks[index], index);
    }
}

std::uint64_t read_be64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

std::uint32_t read_be32_or_zero(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return read_be32(bytes, offset);
}

std::uint32_t track_id_from_trex(const Mp4Box& trex, std::span<const std::uint8_t> bytes) {
    return read_be32_or_zero(bytes, trex.payload.offset + 4);
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

std::vector<std::uint8_t> make_box(std::string_view type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    append_ascii(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> make_full_box(std::string_view type,
                                        std::uint8_t version,
                                        std::uint32_t flags,
                                        const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> full_payload;
    full_payload.push_back(version);
    full_payload.push_back(static_cast<std::uint8_t>((flags >> 16U) & 0xFFU));
    full_payload.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xFFU));
    full_payload.push_back(static_cast<std::uint8_t>(flags & 0xFFU));
    full_payload.insert(full_payload.end(), payload.begin(), payload.end());
    return make_box(type, full_payload);
}

std::vector<std::uint8_t> concat_boxes(const std::vector<std::vector<std::uint8_t>>& boxes) {
    std::vector<std::uint8_t> out;
    for (const auto& box : boxes) {
        out.insert(out.end(), box.begin(), box.end());
    }
    return out;
}

const Mp4Box* require_child(const Mp4Box& box, std::string_view type) {
    const Mp4Box* child = find_child_box(box, type);
    if (child == nullptr) {
        throw std::runtime_error("missing required child box: " + std::string(type));
    }
    return child;
}

std::vector<std::uint32_t> parse_sample_sizes(const Mp4Box& stsz, std::span<const std::uint8_t> bytes) {
    const std::size_t table_offset = stsz.payload.offset + 4;
    const std::uint32_t sample_size = read_be32(bytes, table_offset);
    const std::uint32_t sample_count = read_be32(bytes, table_offset + 4);

    std::vector<std::uint32_t> sizes;
    sizes.reserve(sample_count);

    if (sample_size != 0) {
        sizes.assign(sample_count, sample_size);
        return sizes;
    }

    std::size_t cursor = table_offset + 8;
    for (std::uint32_t index = 0; index < sample_count; ++index) {
        sizes.push_back(read_be32(bytes, cursor));
        cursor += 4;
    }

    return sizes;
}

std::vector<std::uint64_t> parse_chunk_offsets(const Mp4Box& stbl, std::span<const std::uint8_t> bytes) {
    if (const Mp4Box* stco = find_child_box(stbl, "stco")) {
        const std::size_t table_offset = stco->payload.offset + 4;
        const std::uint32_t count = read_be32(bytes, table_offset);
        std::vector<std::uint64_t> offsets;
        offsets.reserve(count);
        std::size_t cursor = table_offset + 4;
        for (std::uint32_t index = 0; index < count; ++index) {
            offsets.push_back(read_be32(bytes, cursor));
            cursor += 4;
        }
        return offsets;
    }

    if (const Mp4Box* co64 = find_child_box(stbl, "co64")) {
        const std::size_t table_offset = co64->payload.offset + 4;
        const std::uint32_t count = read_be32(bytes, table_offset);
        std::vector<std::uint64_t> offsets;
        offsets.reserve(count);
        std::size_t cursor = table_offset + 4;
        for (std::uint32_t index = 0; index < count; ++index) {
            offsets.push_back(read_be64(bytes, cursor));
            cursor += 8;
        }
        return offsets;
    }

    throw std::runtime_error("missing stco/co64 box for progressive MP4 remux");
}

std::vector<ChunkMapEntry> parse_chunk_map(const Mp4Box& stsc, std::span<const std::uint8_t> bytes) {
    const std::size_t table_offset = stsc.payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::vector<ChunkMapEntry> entries;
    entries.reserve(count);
    std::size_t cursor = table_offset + 4;
    for (std::uint32_t index = 0; index < count; ++index) {
        entries.push_back({
            .first_chunk = read_be32(bytes, cursor),
            .samples_per_chunk = read_be32(bytes, cursor + 4),
        });
        cursor += 12;
    }
    return entries;
}

std::vector<std::uint32_t> parse_timing_table(const Mp4Box& stts, std::span<const std::uint8_t> bytes) {
    const std::size_t table_offset = stts.payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::vector<std::uint32_t> durations;
    std::size_t cursor = table_offset + 4;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t run_count = read_be32(bytes, cursor);
        const std::uint32_t delta = read_be32(bytes, cursor + 4);
        durations.insert(durations.end(), run_count, delta);
        cursor += 8;
    }
    return durations;
}

std::vector<std::int32_t> parse_composition_offsets(const Mp4Box* ctts,
                                                    std::size_t sample_count,
                                                    std::span<const std::uint8_t> bytes) {
    std::vector<std::int32_t> offsets(sample_count, 0);
    if (ctts == nullptr) {
        return offsets;
    }

    const std::uint8_t version = bytes[ctts->payload.offset];
    const std::size_t table_offset = ctts->payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::size_t cursor = table_offset + 4;
    std::size_t sample_index = 0;

    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t run_count = read_be32(bytes, cursor);
        const std::uint32_t raw_value = read_be32(bytes, cursor + 4);
        const std::int32_t value =
            version == 1 ? static_cast<std::int32_t>(raw_value) : static_cast<std::int32_t>(raw_value);
        for (std::uint32_t run = 0; run < run_count && sample_index < offsets.size(); ++run) {
            offsets[sample_index++] = value;
        }
        cursor += 8;
    }

    return offsets;
}

FragmentTiming fragment_timing(const Mp4Box& moof,
                               const std::vector<TrackDescription>& tracks,
                               const std::vector<Mp4Box>& top_level_boxes,
                               std::span<const std::uint8_t> bytes) {
    const TrackDescription* track = fragment_track_description(moof, tracks, bytes);
    if (track == nullptr || track->timescale == 0) {
        return {};
    }

    const Mp4Box* traf = find_child_box(moof, "traf");
    if (traf == nullptr) {
        return {
            .start_time_us = 0,
            .duration_us = 0,
            .earliest_presentation_time_us = 0,
            .sap_type = static_cast<std::uint8_t>(track->handler_type == "vide" ? 2 : 1),
        };
    }

    const std::uint32_t track_id = track->track_id;

    std::uint64_t base_decode_time = 0;
    if (const Mp4Box* tfdt = find_child_box(*traf, "tfdt")) {
        const std::uint8_t version = bytes[tfdt->payload.offset];
        const std::size_t time_offset = tfdt->payload.offset + 4;
        if (version == 1 && time_offset + 8 <= bytes.size()) {
            base_decode_time = read_be64(bytes, time_offset);
        } else if (time_offset + 4 <= bytes.size()) {
            base_decode_time = read_be32(bytes, time_offset);
        }
    }

    std::uint32_t default_sample_duration = 0;
    std::uint32_t default_sample_flags = 0x02000000U;
    if (const Mp4Box* tfhd = find_child_box(*traf, "tfhd")) {
        const std::uint32_t flags = read_full_box_flags(*tfhd, bytes);
        std::size_t cursor = tfhd->payload.offset + 8;
        if ((flags & 0x000001U) != 0) {
            cursor += 8;
        }
        if ((flags & 0x000002U) != 0) {
            cursor += 4;
        }
        if ((flags & 0x000008U) != 0 && cursor + 4 <= bytes.size()) {
            default_sample_duration = read_be32(bytes, cursor);
            cursor += 4;
        }
        if ((flags & 0x000010U) != 0 && cursor + 4 <= bytes.size()) {
            cursor += 4;
        }
        if ((flags & 0x000020U) != 0 && cursor + 4 <= bytes.size()) {
            default_sample_flags = read_be32(bytes, cursor);
        }
    }

    if (const Mp4Box* moov = find_first_box(top_level_boxes, "moov")) {
        if (const Mp4Box* mvex = find_child_box(*moov, "mvex")) {
            for (const auto& child : mvex->children) {
                if (child.type != "trex") {
                    continue;
                }
                if (track_id_from_trex(child, bytes) != track_id) {
                    continue;
                }
                default_sample_flags = read_be32_or_zero(bytes, child.payload.offset + 20);
                break;
            }
        }
    }

    std::uint64_t duration = 0;
    std::uint64_t earliest_presentation_time = base_decode_time;
    bool earliest_presentation_time_set = false;
    std::uint32_t first_sample_flags = default_sample_flags;
    if (const Mp4Box* trun = find_child_box(*traf, "trun")) {
        const std::uint32_t flags = read_full_box_flags(*trun, bytes);
        std::size_t cursor = trun->payload.offset + 4;
        if (cursor + 4 <= bytes.size()) {
            const std::uint32_t sample_count = read_be32(bytes, cursor);
            cursor += 4;
            if ((flags & 0x000001U) != 0) {
                cursor += 4;
            }
            bool first_sample_flags_present = false;
            if ((flags & 0x000004U) != 0) {
                first_sample_flags = read_be32_or_zero(bytes, cursor);
                first_sample_flags_present = true;
                cursor += 4;
            }
            std::uint64_t sample_decode_time = base_decode_time;
            for (std::uint32_t index = 0; index < sample_count && cursor <= bytes.size(); ++index) {
                std::uint32_t sample_duration = default_sample_duration;
                if ((flags & 0x000100U) != 0 && cursor + 4 <= bytes.size()) {
                    sample_duration = read_be32(bytes, cursor);
                    cursor += 4;
                }
                std::uint32_t sample_flags = first_sample_flags_present && index == 0 ? first_sample_flags : default_sample_flags;
                if ((flags & 0x000400U) != 0 && cursor + 4 <= bytes.size()) {
                    sample_flags = read_be32(bytes, cursor);
                    cursor += 4;
                }
                std::int32_t composition_offset = 0;
                if ((flags & 0x000800U) != 0 && cursor + 4 <= bytes.size()) {
                    composition_offset = static_cast<std::int32_t>(read_be32(bytes, cursor));
                    cursor += 4;
                }
                const std::int64_t presentation_time_signed =
                    static_cast<std::int64_t>(sample_decode_time) + static_cast<std::int64_t>(composition_offset);
                const std::uint64_t presentation_time =
                    presentation_time_signed < 0 ? 0 : static_cast<std::uint64_t>(presentation_time_signed);
                if (!earliest_presentation_time_set || presentation_time < earliest_presentation_time) {
                    earliest_presentation_time = presentation_time;
                    earliest_presentation_time_set = true;
                }
                if (index == 0 && (flags & 0x000400U) != 0) {
                    first_sample_flags = sample_flags;
                }
                duration += sample_duration;
                if ((flags & 0x000200U) != 0) {
                    cursor += 4;
                }
                sample_decode_time += sample_duration;
            }
        }
    }

    std::uint8_t sap_type = 0;
    const bool first_sample_is_sync = (first_sample_flags & 0x00010000U) == 0;
    if (track->handler_type != "vide") {
        sap_type = 1;
    } else if (first_sample_is_sync) {
        sap_type = 2;
    }

    return {
        .start_time_us = scale_to_us(base_decode_time, track->timescale),
        .duration_us = scale_to_us(duration, track->timescale),
        .earliest_presentation_time_us = scale_to_us(earliest_presentation_time, track->timescale),
        .sap_type = sap_type,
    };
}

std::vector<bool> parse_sync_samples(const Mp4Box* stss,
                                     std::size_t sample_count,
                                     std::string_view handler_type,
                                     std::span<const std::uint8_t> bytes) {
    std::vector<bool> sync_samples(sample_count, handler_type != "vide");
    if (stss == nullptr) {
        return sync_samples;
    }

    std::fill(sync_samples.begin(), sync_samples.end(), false);
    const std::size_t table_offset = stss->payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::size_t cursor = table_offset + 4;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t sample_number = read_be32(bytes, cursor);
        if (sample_number > 0 && sample_number <= sync_samples.size()) {
            sync_samples[sample_number - 1] = true;
        }
        cursor += 4;
    }
    return sync_samples;
}

std::vector<std::uint64_t> derive_sample_offsets(const std::vector<std::uint64_t>& chunk_offsets,
                                                 const std::vector<ChunkMapEntry>& chunk_map,
                                                 const std::vector<std::uint32_t>& sample_sizes) {
    std::vector<std::uint64_t> sample_offsets;
    sample_offsets.reserve(sample_sizes.size());

    std::size_t sample_index = 0;
    std::size_t map_index = 0;

    for (std::size_t chunk_index = 0; chunk_index < chunk_offsets.size(); ++chunk_index) {
        while (map_index + 1 < chunk_map.size() && chunk_map[map_index + 1].first_chunk <= chunk_index + 1) {
            ++map_index;
        }

        std::uint64_t current_offset = chunk_offsets[chunk_index];
        for (std::uint32_t in_chunk = 0; in_chunk < chunk_map[map_index].samples_per_chunk; ++in_chunk) {
            if (sample_index >= sample_sizes.size()) {
                break;
            }
            sample_offsets.push_back(current_offset);
            current_offset += sample_sizes[sample_index++];
        }
    }

    if (sample_offsets.size() != sample_sizes.size()) {
        throw std::runtime_error("sample table mapping does not cover all samples");
    }

    return sample_offsets;
}

std::uint32_t parse_timescale(const Mp4Box& mdhd, std::span<const std::uint8_t> bytes) {
    const std::uint8_t version = bytes[mdhd.payload.offset];
    const std::size_t offset = mdhd.payload.offset + (version == 1 ? 20 : 12);
    return read_be32(bytes, offset);
}

TrackRemuxInfo parse_track_remux_info(const Mp4Box& trak,
                                     const TrackDescription& track,
                                     std::span<const std::uint8_t> bytes) {
    const Mp4Box* mdia = require_child(trak, "mdia");
    const Mp4Box* mdhd = require_child(*mdia, "mdhd");
    const Mp4Box* minf = require_child(*mdia, "minf");
    const Mp4Box* stbl = require_child(*minf, "stbl");
    const Mp4Box* stsz = require_child(*stbl, "stsz");
    const Mp4Box* stsc = require_child(*stbl, "stsc");
    const Mp4Box* stts = require_child(*stbl, "stts");

    const std::vector<std::uint32_t> sample_sizes = parse_sample_sizes(*stsz, bytes);
    const std::vector<std::uint32_t> sample_durations = parse_timing_table(*stts, bytes);
    if (sample_sizes.size() != sample_durations.size()) {
        throw std::runtime_error("stsz and stts tables disagree on sample count");
    }

    return {
        .description = track,
        .timescale = parse_timescale(*mdhd, bytes),
        .sample_offsets = derive_sample_offsets(parse_chunk_offsets(*stbl, bytes),
                                               parse_chunk_map(*stsc, bytes),
                                               sample_sizes),
        .sample_sizes = sample_sizes,
        .sample_durations = sample_durations,
        .composition_offsets = parse_composition_offsets(find_child_box(*stbl, "ctts"), sample_sizes.size(), bytes),
        .sync_samples = parse_sync_samples(find_child_box(*stbl, "stss"), sample_sizes.size(), track.handler_type, bytes),
    };
}

std::vector<std::uint8_t> build_mvex_box(const std::vector<TrackDescription>& tracks) {
    std::vector<std::vector<std::uint8_t>> trex_boxes;
    trex_boxes.reserve(tracks.size());
    for (const auto& track : tracks) {
        std::vector<std::uint8_t> trex_payload;
        append_be32(trex_payload, track.track_id);
        append_be32(trex_payload, 1);
        append_be32(trex_payload, 0);
        append_be32(trex_payload, 0);
        append_be32(trex_payload, 0);
        trex_boxes.push_back(make_full_box("trex", 0, 0, trex_payload));
    }
    return make_box("mvex", concat_boxes(trex_boxes));
}

std::vector<std::uint8_t> build_fragmented_init_segment(const Mp4Box& ftyp,
                                                        const Mp4Box& moov,
                                                        const std::vector<TrackDescription>& tracks,
                                                        std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> init(slice_bytes(bytes, ftyp.span).begin(), slice_bytes(bytes, ftyp.span).end());
    std::vector<std::uint8_t> moov_bytes(slice_bytes(bytes, moov.span).begin(), slice_bytes(bytes, moov.span).end());

    if (find_child_box(moov, "mvex") == nullptr) {
        const std::vector<std::uint8_t> mvex = build_mvex_box(tracks);
        moov_bytes.insert(moov_bytes.end(), mvex.begin(), mvex.end());
        const std::uint32_t new_size = static_cast<std::uint32_t>(moov_bytes.size());
        moov_bytes[0] = static_cast<std::uint8_t>((new_size >> 24U) & 0xFFU);
        moov_bytes[1] = static_cast<std::uint8_t>((new_size >> 16U) & 0xFFU);
        moov_bytes[2] = static_cast<std::uint8_t>((new_size >> 8U) & 0xFFU);
        moov_bytes[3] = static_cast<std::uint8_t>(new_size & 0xFFU);
    }

    init.insert(init.end(), moov_bytes.begin(), moov_bytes.end());
    patch_init_segment_sample_entries(init, tracks);
    return init;
}

std::uint32_t sample_flags_for(const TrackRemuxInfo& track, std::size_t sample_index) {
    if (track.description.handler_type == "vide") {
        return track.sync_samples[sample_index] ? 0x02000000U : 0x01010000U;
    }
    return 0x02000000U;
}

std::vector<std::uint8_t> build_trun_range_box(const TrackRemuxInfo& track,
                                               std::size_t first, std::size_t count,
                                               std::uint32_t data_offset) {
    std::vector<std::uint8_t> trun_payload;
    append_be32(trun_payload, static_cast<std::uint32_t>(count));
    append_be32(trun_payload, data_offset);
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t index = first + k;
        append_be32(trun_payload, track.sample_durations[index]);
        append_be32(trun_payload, track.sample_sizes[index]);
        append_be32(trun_payload, sample_flags_for(track, index));
        append_be32(trun_payload, static_cast<std::uint32_t>(track.composition_offsets[index]));
    }
    return make_full_box("trun", 1, 0x000F01, trun_payload);
}

std::vector<std::uint8_t> build_single_sample_trun_box(const SampleObjectInfo& sample, std::uint32_t data_offset) {
    std::vector<std::uint8_t> trun_payload;
    append_be32(trun_payload, 1);
    append_be32(trun_payload, data_offset);
    append_be32(trun_payload, sample.duration);
    append_be32(trun_payload, sample.size);
    append_be32(trun_payload, sample.flags);
    append_be32(trun_payload, static_cast<std::uint32_t>(sample.composition_offset));
    return make_full_box("trun", 1, 0x000F01, trun_payload);
}

std::vector<std::uint8_t> build_sample_object(std::uint32_t track_id,
                                              std::size_t sequence,
                                              const SampleObjectInfo& sample,
                                              std::span<const std::uint8_t> bytes) {
    const auto sample_bytes = slice_bytes(bytes, sample.payload_span);
    std::vector<std::uint8_t> mdat_payload(sample_bytes.begin(), sample_bytes.end());

    std::vector<std::uint8_t> mfhd_payload;
    append_be32(mfhd_payload, static_cast<std::uint32_t>(sequence + 1));
    const std::vector<std::uint8_t> mfhd = make_full_box("mfhd", 0, 0, mfhd_payload);

    std::vector<std::uint8_t> tfhd_payload;
    append_be32(tfhd_payload, track_id);
    const std::vector<std::uint8_t> tfhd = make_full_box("tfhd", 0, 0x020000, tfhd_payload);

    std::vector<std::uint8_t> tfdt_payload;
    append_be32(tfdt_payload, static_cast<std::uint32_t>(sample.decode_time));
    const std::vector<std::uint8_t> tfdt = make_full_box("tfdt", 0, 0, tfdt_payload);

    const std::vector<std::uint8_t> placeholder_trun = build_single_sample_trun_box(sample, 0);
    const std::vector<std::uint8_t> placeholder_traf = make_box("traf", concat_boxes({tfhd, tfdt, placeholder_trun}));
    const std::vector<std::uint8_t> placeholder_moof = make_box("moof", concat_boxes({mfhd, placeholder_traf}));
    const std::uint32_t data_offset = static_cast<std::uint32_t>(placeholder_moof.size() + 8);

    const std::vector<std::uint8_t> trun = build_single_sample_trun_box(sample, data_offset);
    const std::vector<std::uint8_t> traf = make_box("traf", concat_boxes({tfhd, tfdt, trun}));
    const std::vector<std::uint8_t> moof = make_box("moof", concat_boxes({mfhd, traf}));
    const std::vector<std::uint8_t> mdat = make_box("mdat", mdat_payload);

    return concat_boxes({moof, mdat});
}

// Build one CMAF fragment (moof+mdat) covering samples [first, first+count) of a
// progressive track, with tfdt = base_decode_time. Used by bounded per-GOP
// coalescing: each fragment carries a subrange of the track's samples (one GOP,
// or a capped slice of a long GOP) rather than the whole track.
std::vector<std::uint8_t> build_remuxed_fragment_range(const TrackRemuxInfo& track,
                                                       std::size_t first, std::size_t count,
                                                       std::uint64_t base_decode_time,
                                                       std::size_t sequence,
                                                       std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> mdat_payload;
    std::size_t total = 0;
    for (std::size_t k = 0; k < count; ++k) total += track.sample_sizes[first + k];
    mdat_payload.reserve(total);
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t index = first + k;
        const ByteSpan sample_span{.offset = static_cast<std::size_t>(track.sample_offsets[index]),
                                   .size = track.sample_sizes[index]};
        const auto sample_bytes = slice_bytes(bytes, sample_span);
        mdat_payload.insert(mdat_payload.end(), sample_bytes.begin(), sample_bytes.end());
    }

    std::vector<std::uint8_t> mfhd_payload;
    append_be32(mfhd_payload, static_cast<std::uint32_t>(sequence + 1));
    const std::vector<std::uint8_t> mfhd = make_full_box("mfhd", 0, 0, mfhd_payload);

    std::vector<std::uint8_t> tfhd_payload;
    append_be32(tfhd_payload, track.description.track_id);
    const std::vector<std::uint8_t> tfhd = make_full_box("tfhd", 0, 0x020000, tfhd_payload);

    std::vector<std::uint8_t> tfdt_payload;
    append_be32(tfdt_payload, static_cast<std::uint32_t>(base_decode_time));
    const std::vector<std::uint8_t> tfdt = make_full_box("tfdt", 0, 0, tfdt_payload);

    const std::vector<std::uint8_t> placeholder_trun = build_trun_range_box(track, first, count, 0);
    const std::vector<std::uint8_t> placeholder_traf = make_box("traf", concat_boxes({tfhd, tfdt, placeholder_trun}));
    const std::vector<std::uint8_t> placeholder_moof = make_box("moof", concat_boxes({mfhd, placeholder_traf}));
    const std::uint32_t data_offset = static_cast<std::uint32_t>(placeholder_moof.size() + 8);

    const std::vector<std::uint8_t> trun = build_trun_range_box(track, first, count, data_offset);
    const std::vector<std::uint8_t> traf = make_box("traf", concat_boxes({tfhd, tfdt, trun}));
    const std::vector<std::uint8_t> moof = make_box("moof", concat_boxes({mfhd, traf}));
    const std::vector<std::uint8_t> mdat = make_box("mdat", mdat_payload);

    return concat_boxes({moof, mdat});
}

std::uint8_t sap_type_from_flags(std::string_view handler_type, std::uint32_t sample_flags) {
    if (handler_type != "vide") {
        return 1;
    }
    return (sample_flags & 0x00010000U) == 0 ? 2 : 0;
}

std::uint64_t presentation_time_us(std::uint64_t decode_time,
                                   std::int32_t composition_offset,
                                   std::uint32_t timescale) {
    const std::int64_t presentation_time_signed =
        static_cast<std::int64_t>(decode_time) + static_cast<std::int64_t>(composition_offset);
    return scale_to_us(presentation_time_signed < 0 ? 0 : static_cast<std::uint64_t>(presentation_time_signed),
                       timescale);
}

std::vector<SampleObjectInfo> parse_fragment_samples(const Mp4Box& moof,
                                                     const Mp4Box& mdat,
                                                     const TrackDescription& track,
                                                     const std::vector<Mp4Box>& top_level_boxes,
                                                     std::span<const std::uint8_t> bytes) {
    const Mp4Box* traf = find_child_box(moof, "traf");
    const Mp4Box* trun = traf == nullptr ? nullptr : find_child_box(*traf, "trun");
    if (traf == nullptr || trun == nullptr) {
        return {};
    }

    std::uint64_t base_decode_time = 0;
    if (const Mp4Box* tfdt = find_child_box(*traf, "tfdt")) {
        const std::uint8_t version = bytes[tfdt->payload.offset];
        const std::size_t time_offset = tfdt->payload.offset + 4;
        if (version == 1 && time_offset + 8 <= bytes.size()) {
            base_decode_time = read_be64(bytes, time_offset);
        } else if (time_offset + 4 <= bytes.size()) {
            base_decode_time = read_be32(bytes, time_offset);
        }
    }

    std::uint32_t default_sample_duration = 0;
    std::uint32_t default_sample_flags = 0x02000000U;
    if (const Mp4Box* tfhd = find_child_box(*traf, "tfhd")) {
        const std::uint32_t flags = read_full_box_flags(*tfhd, bytes);
        std::size_t cursor = tfhd->payload.offset + 8;
        if ((flags & 0x000001U) != 0) {
            cursor += 8;
        }
        if ((flags & 0x000002U) != 0) {
            cursor += 4;
        }
        if ((flags & 0x000008U) != 0 && cursor + 4 <= bytes.size()) {
            default_sample_duration = read_be32(bytes, cursor);
            cursor += 4;
        }
        if ((flags & 0x000010U) != 0 && cursor + 4 <= bytes.size()) {
            cursor += 4;
        }
        if ((flags & 0x000020U) != 0 && cursor + 4 <= bytes.size()) {
            default_sample_flags = read_be32(bytes, cursor);
        }
    }
    if (const Mp4Box* moov = find_first_box(top_level_boxes, "moov")) {
        if (const Mp4Box* mvex = find_child_box(*moov, "mvex")) {
            for (const auto& child : mvex->children) {
                if (child.type == "trex" && track_id_from_trex(child, bytes) == track.track_id) {
                    default_sample_flags = read_be32_or_zero(bytes, child.payload.offset + 20);
                    break;
                }
            }
        }
    }

    const std::uint32_t trun_flags = read_full_box_flags(*trun, bytes);
    std::size_t cursor = trun->payload.offset + 4;
    if (cursor + 4 > bytes.size()) {
        return {};
    }
    const std::uint32_t sample_count = read_be32(bytes, cursor);
    cursor += 4;
    if ((trun_flags & 0x000001U) != 0) {
        cursor += 4;
    }
    std::uint32_t first_sample_flags = default_sample_flags;
    bool first_sample_flags_present = false;
    if ((trun_flags & 0x000004U) != 0 && cursor + 4 <= bytes.size()) {
        first_sample_flags = read_be32(bytes, cursor);
        first_sample_flags_present = true;
        cursor += 4;
    }

    std::vector<SampleObjectInfo> samples;
    samples.reserve(sample_count);
    std::uint64_t sample_decode_time = base_decode_time;
    std::size_t payload_offset = mdat.payload.offset;
    for (std::uint32_t index = 0; index < sample_count && cursor <= bytes.size(); ++index) {
        std::uint32_t sample_duration = default_sample_duration;
        if ((trun_flags & 0x000100U) != 0 && cursor + 4 <= bytes.size()) {
            sample_duration = read_be32(bytes, cursor);
            cursor += 4;
        }
        std::uint32_t sample_size = 0;
        if ((trun_flags & 0x000200U) != 0 && cursor + 4 <= bytes.size()) {
            sample_size = read_be32(bytes, cursor);
            cursor += 4;
        }
        std::uint32_t sample_flags = first_sample_flags_present && index == 0 ? first_sample_flags : default_sample_flags;
        if ((trun_flags & 0x000400U) != 0 && cursor + 4 <= bytes.size()) {
            sample_flags = read_be32(bytes, cursor);
            cursor += 4;
        }
        std::int32_t composition_offset = 0;
        if ((trun_flags & 0x000800U) != 0 && cursor + 4 <= bytes.size()) {
            composition_offset = static_cast<std::int32_t>(read_be32(bytes, cursor));
            cursor += 4;
        }
        samples.push_back({
            .decode_time = sample_decode_time,
            .duration = sample_duration,
            .size = sample_size,
            .flags = sample_flags,
            .composition_offset = composition_offset,
            .payload_span = {.offset = payload_offset, .size = sample_size},
        });
        payload_offset += sample_size;
        sample_decode_time += sample_duration;
    }
    return samples;
}

// Cap on samples packed into a single coalesced CMAF object. Kept comfortably
// below libmoq's CMAF validator scratch buffer (512 samples per trun) so a long
// GOP is split across continuation objects within its group rather than
// producing one oversized fragment that the validator rejects.
constexpr std::size_t kMaxSamplesPerCoalescedObject = 120;

// Decode-time (microseconds) at which each video GOP begins, used to align audio
// group boundaries to the video group timeline.
std::vector<std::uint64_t> video_group_start_times_us(const TrackRemuxInfo& video) {
    std::vector<std::uint64_t> starts;
    std::uint64_t decode_time = 0;
    for (std::size_t i = 0; i < video.sample_sizes.size(); ++i) {
        if (i == 0 || video.sync_samples[i]) {
            starts.push_back(scale_to_us(decode_time, video.timescale));
        }
        decode_time += video.sample_durations[i];
    }
    return starts;
}

// Bounded coalescing for one progressive track. Video groups break at sync
// samples (each GOP -> one group whose first object is a SAP); audio/other
// groups break at the video group boundaries (or form a single group when there
// is no video reference). Each group is then chunked into objects of at most
// kMaxSamplesPerCoalescedObject samples. The track forms ONE MoQ group (like
// the split path: group_id = track_index), so the relay forwards a single
// long-lived subgroup rather than a burst of short-lived per-GOP groups -- the
// latter does not survive a lazy relay's subscribe/forward window. GOP/segment
// boundaries become OBJECT boundaries within that group: object 0 begins with
// the track's first keyframe (the group-start SAP); each later video object
// that begins a GOP is also a SAP, and continuation objects (a long GOP split
// to stay under the validator buffer) are non-SAP. This replaces the previous
// whole-track object, which produced one trun over every sample and tripped
// libmoq's 512-sample CMAF validator.
void append_coalesced_fragments(SegmentedMp4& segmented,
                                const TrackRemuxInfo& track,
                                std::size_t track_index,
                                const std::vector<std::uint64_t>& video_group_starts_us,
                                std::span<const std::uint8_t> bytes) {
    const std::size_t n = track.sample_sizes.size();
    if (n == 0) {
        return;
    }
    const bool is_video = track.description.handler_type == "vide";

    // Prefix sum of sample durations -> decode time (timescale units) per sample.
    std::vector<std::uint64_t> decode_time(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        decode_time[i + 1] = decode_time[i] + track.sample_durations[i];
    }

    // GOP/segment boundaries: video breaks at sync samples; audio/other breaks
    // at the video GOP timeline (each becomes an object boundary, not a group).
    std::vector<std::size_t> gop_starts;
    if (is_video) {
        for (std::size_t i = 0; i < n; ++i) {
            if (i == 0 || track.sync_samples[i]) {
                gop_starts.push_back(i);
            }
        }
    } else if (!video_group_starts_us.empty()) {
        std::size_t vg = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t t_us = scale_to_us(decode_time[i], track.timescale);
            bool start = (i == 0);
            while (vg + 1 < video_group_starts_us.size() &&
                   t_us >= video_group_starts_us[vg + 1]) {
                ++vg;
                start = true;
            }
            if (start) {
                gop_starts.push_back(i);
            }
        }
    }
    if (gop_starts.empty() || gop_starts.front() != 0) {
        gop_starts.insert(gop_starts.begin(), 0);
    }

    std::size_t object_id = 0;
    for (std::size_t g = 0; g < gop_starts.size(); ++g) {
        const std::size_t gop_begin = gop_starts[g];
        const std::size_t gop_end = (g + 1 < gop_starts.size()) ? gop_starts[g + 1] : n;

        for (std::size_t c0 = gop_begin; c0 < gop_end; c0 += kMaxSamplesPerCoalescedObject) {
            const std::size_t c1 = std::min(c0 + kMaxSamplesPerCoalescedObject, gop_end);
            const std::size_t count = c1 - c0;
            const std::uint64_t base_decode_time = decode_time[c0];
            const std::uint64_t duration = decode_time[c1] - decode_time[c0];

            // A video object that begins a GOP (c0 == gop_begin) starts with the
            // keyframe -> SAP type 2; a continuation chunk of a long GOP is
            // non-SAP. Every audio object is a SAP (type 1). object 0 is the
            // group-start and is always a SAP, satisfying CMSF Section 3.4.
            const std::uint8_t sap = is_video
                ? static_cast<std::uint8_t>(c0 == gop_begin ? 2 : 0)
                : static_cast<std::uint8_t>(1);

            segmented.fragments.push_back({
                .group_id = track_index,
                .object_id = object_id,
                .track_name = track.description.track_name,
                .start_time_us = scale_to_us(base_decode_time, track.timescale),
                .duration_us = scale_to_us(duration, track.timescale),
                .earliest_presentation_time_us =
                    presentation_time_us(base_decode_time, track.composition_offsets[c0], track.timescale),
                .sap_type = sap,
                .has_sap_type = true,
                .payload = {.span = {},
                            .owned_bytes = build_remuxed_fragment_range(track, c0, count,
                                                                        base_decode_time, object_id, bytes)},
            });
            ++object_id;
        }
    }
}

}  // namespace

SegmentedMp4 segment_for_cmaf(const ParsedMp4& parsed_mp4, CmafObjectMode object_mode) {
    const Mp4Box* ftyp = find_first_box(parsed_mp4.top_level_boxes, "ftyp");
    const Mp4Box* moov = find_first_box(parsed_mp4.top_level_boxes, "moov");
    if (ftyp == nullptr || moov == nullptr) {
        throw std::runtime_error("input must contain ftyp and moov boxes");
    }

    SegmentedMp4 segmented{
        .initialization_segment = {.span = {.offset = ftyp->span.offset,
                                            .size = moov->span.offset + moov->span.size - ftyp->span.offset},
                                   .owned_bytes = {}},
        .fragments = {},
        .tracks = parsed_mp4.tracks,
    };

    const std::vector<const Mp4Box*> moofs = find_boxes(parsed_mp4.top_level_boxes, "moof");
    const std::vector<const Mp4Box*> mdats = find_boxes(parsed_mp4.top_level_boxes, "mdat");

    if (!moofs.empty()) {
        segmented.initialization_segment.owned_bytes.assign(
            slice_bytes(parsed_mp4.bytes, segmented.initialization_segment.span).begin(),
            slice_bytes(parsed_mp4.bytes, segmented.initialization_segment.span).end());
        patch_init_segment_sample_entries(segmented.initialization_segment.owned_bytes, parsed_mp4.tracks);
        segmented.initialization_segment.span = {};

        if (moofs.size() != mdats.size()) {
            throw std::runtime_error("fragmented MP4 must contain matched moof/mdat pairs");
        }

        std::map<std::string, std::size_t> next_group_by_track;
        for (std::size_t index = 0; index < moofs.size(); ++index) {
            if (moofs[index]->span.offset > mdats[index]->span.offset) {
                throw std::runtime_error("expected moof to precede matching mdat");
            }
            const std::string track_name = fragment_track_name(*moofs[index], parsed_mp4.tracks, parsed_mp4.bytes);
            const FragmentTiming timing = fragment_timing(*moofs[index], parsed_mp4.tracks, parsed_mp4.top_level_boxes, parsed_mp4.bytes);
            const std::size_t group_id = next_group_by_track[track_name]++;

            const auto track_it = std::find_if(parsed_mp4.tracks.begin(), parsed_mp4.tracks.end(), [&](const TrackDescription& track) {
                return track.track_name == track_name;
            });
            if (object_mode == CmafObjectMode::kSplit && track_it != parsed_mp4.tracks.end()) {
                const std::vector<SampleObjectInfo> samples =
                    parse_fragment_samples(*moofs[index], *mdats[index], *track_it, parsed_mp4.top_level_boxes, parsed_mp4.bytes);
                if (!samples.empty()) {
                    for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
                        const auto& sample = samples[sample_index];
                        segmented.fragments.push_back({
                            .group_id = group_id,
                            .object_id = sample_index,
                            .track_name = track_name,
                            .start_time_us = scale_to_us(sample.decode_time, track_it->timescale),
                            .duration_us = scale_to_us(sample.duration, track_it->timescale),
                            .earliest_presentation_time_us =
                                presentation_time_us(sample.decode_time, sample.composition_offset, track_it->timescale),
                            .sap_type = sap_type_from_flags(track_it->handler_type, sample.flags),
                            .has_sap_type = true,
                            .payload = {.span = {},
                                        .owned_bytes = build_sample_object(track_it->track_id,
                                                                           group_id * 1000 + sample_index,
                                                                           sample,
                                                                           parsed_mp4.bytes)},
                        });
                    }
                    continue;
                }
            }

            segmented.fragments.push_back({
                .group_id = group_id,
                .object_id = 0,
                .track_name = track_name,
                .start_time_us = timing.start_time_us,
                .duration_us = timing.duration_us,
                .earliest_presentation_time_us = timing.earliest_presentation_time_us,
                .sap_type = timing.sap_type,
                .has_sap_type = true,
                .payload = {.span = {.offset = moofs[index]->span.offset,
                                     .size = moofs[index]->span.size + mdats[index]->span.size},
                            .owned_bytes = {}},
            });
        }

        return segmented;
    }

    segmented.initialization_segment.owned_bytes =
        build_fragmented_init_segment(*ftyp, *moov, parsed_mp4.tracks, parsed_mp4.bytes);
    segmented.initialization_segment.span = {};

    // For bounded coalescing, audio group boundaries follow the video GOP
    // timeline, so derive the video group start times before segmenting tracks.
    std::vector<std::uint64_t> video_group_starts_us;
    if (object_mode != CmafObjectMode::kSplit) {
        std::size_t scan_index = 0;
        for (const auto& child : moov->children) {
            if (child.type != "trak") {
                continue;
            }
            if (scan_index >= parsed_mp4.tracks.size()) {
                break;
            }
            if (parsed_mp4.tracks[scan_index].handler_type == "vide") {
                const TrackRemuxInfo video_info =
                    parse_track_remux_info(child, parsed_mp4.tracks[scan_index], parsed_mp4.bytes);
                video_group_starts_us = video_group_start_times_us(video_info);
                break;
            }
            ++scan_index;
        }
    }

    std::size_t track_index = 0;
    for (const auto& child : moov->children) {
        if (child.type != "trak") {
            continue;
        }
        if (track_index >= parsed_mp4.tracks.size()) {
            break;
        }

        const TrackRemuxInfo track_info = parse_track_remux_info(child, parsed_mp4.tracks[track_index], parsed_mp4.bytes);
        if (object_mode == CmafObjectMode::kSplit) {
            std::uint64_t decode_time = 0;
            for (std::size_t sample_index = 0; sample_index < track_info.sample_sizes.size(); ++sample_index) {
                const SampleObjectInfo sample{
                    .decode_time = decode_time,
                    .duration = track_info.sample_durations[sample_index],
                    .size = track_info.sample_sizes[sample_index],
                    .flags = sample_flags_for(track_info, sample_index),
                    .composition_offset = track_info.composition_offsets[sample_index],
                    .payload_span = {.offset = static_cast<std::size_t>(track_info.sample_offsets[sample_index]),
                                     .size = track_info.sample_sizes[sample_index]},
                };
                segmented.fragments.push_back({
                    .group_id = track_index,
                    .object_id = sample_index,
                    .track_name = track_info.description.track_name,
                    .start_time_us = scale_to_us(sample.decode_time, track_info.timescale),
                    .duration_us = scale_to_us(sample.duration, track_info.timescale),
                    .earliest_presentation_time_us =
                        presentation_time_us(sample.decode_time, sample.composition_offset, track_info.timescale),
                    .sap_type = sap_type_from_flags(track_info.description.handler_type, sample.flags),
                    .has_sap_type = true,
                    .payload = {.span = {},
                                .owned_bytes = build_sample_object(track_info.description.track_id,
                                                                   track_index * 1000 + sample_index,
                                                                   sample,
                                                                   parsed_mp4.bytes)},
                });
                decode_time += sample.duration;
            }
        } else {
            // Bounded keyframe/GOP coalescing: one group per track (like split),
            // with each GOP (video) or aligned span (audio) a separate object,
            // chunked to stay under the 512-sample validator -- never a single
            // whole-track fragment.
            append_coalesced_fragments(segmented, track_info, track_index, video_group_starts_us,
                                       parsed_mp4.bytes);
        }
        ++track_index;
    }

    if (segmented.fragments.empty()) {
        throw std::runtime_error("no tracks available for progressive MP4 remux");
    }

    return segmented;
}

std::string summarize_tracks(const std::vector<TrackDescription>& tracks) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        stream << tracks[index].track_name << "(" << tracks[index].handler_type << ":" << tracks[index].codec << ")";
    }
    return stream.str();
}

std::size_t payload_size(const PayloadBuffer& payload) {
    return payload.owned_bytes.empty() ? payload.span.size : payload.owned_bytes.size();
}

MediaFragment build_live_fragment(std::span<const std::uint8_t> moof_bytes,
                                  std::span<const std::uint8_t> mdat_bytes,
                                  const std::vector<TrackDescription>& tracks,
                                  std::size_t group_id) {
    // Extract timing data directly from the standalone moof bytes.
    // For fragmented MP4 from ffmpeg, tfhd carries default_sample_flags so we
    // do not need to look up trex defaults from the init segment.
    const std::vector<Mp4Box> moof_boxes = parse_mp4_boxes(moof_bytes);
    if (moof_boxes.empty() || moof_boxes.front().type != "moof") {
        throw std::runtime_error("build_live_fragment: expected moof box");
    }
    const Mp4Box& moof = moof_boxes.front();

    // Extract track name from moof -> traf -> tfhd -> track_id.
    const std::string track_name = fragment_track_name(moof, tracks, moof_bytes);

    // Extract timing information directly from moof bytes.
    const TrackDescription* track_desc = fragment_track_description(moof, tracks, moof_bytes);
    if (track_desc == nullptr || track_desc->timescale == 0) {
        throw std::runtime_error("build_live_fragment: cannot find track for fragment");
    }

    const Mp4Box* traf = find_child_box(moof, "traf");
    if (traf == nullptr) {
        throw std::runtime_error("build_live_fragment: moof has no traf");
    }

    std::uint64_t base_decode_time = 0;
    if (const Mp4Box* tfdt = find_child_box(*traf, "tfdt")) {
        const std::uint8_t version = moof_bytes[tfdt->payload.offset];
        const std::size_t time_offset = tfdt->payload.offset + 4;
        if (version == 1 && time_offset + 8 <= moof_bytes.size()) {
            std::uint64_t val = 0;
            for (int i = 0; i < 8; ++i) {
                val = (val << 8U) | moof_bytes[time_offset + i];
            }
            base_decode_time = val;
        } else if (time_offset + 4 <= moof_bytes.size()) {
            base_decode_time = read_be32(moof_bytes, time_offset);
        }
    }

    std::uint32_t default_sample_duration = 0;
    std::uint32_t default_sample_flags = 0x02000000U;
    if (const Mp4Box* tfhd = find_child_box(*traf, "tfhd")) {
        const std::uint32_t flags = read_full_box_flags(*tfhd, moof_bytes);
        std::size_t cursor = tfhd->payload.offset + 8;
        if ((flags & 0x000001U) != 0) cursor += 8;
        if ((flags & 0x000002U) != 0) cursor += 4;
        if ((flags & 0x000008U) != 0 && cursor + 4 <= moof_bytes.size()) {
            default_sample_duration = read_be32(moof_bytes, cursor);
            cursor += 4;
        }
        if ((flags & 0x000010U) != 0 && cursor + 4 <= moof_bytes.size()) {
            cursor += 4;
        }
        if ((flags & 0x000020U) != 0 && cursor + 4 <= moof_bytes.size()) {
            default_sample_flags = read_be32(moof_bytes, cursor);
        }
    }

    std::uint64_t duration = 0;
    std::uint64_t earliest_presentation_time = base_decode_time;
    bool earliest_presentation_time_set = false;
    std::uint32_t first_sample_flags = default_sample_flags;
    if (const Mp4Box* trun = find_child_box(*traf, "trun")) {
        const std::uint32_t flags = read_full_box_flags(*trun, moof_bytes);
        std::size_t cursor = trun->payload.offset + 4;
        if (cursor + 4 <= moof_bytes.size()) {
            const std::uint32_t sample_count = read_be32(moof_bytes, cursor);
            cursor += 4;
            if ((flags & 0x000001U) != 0) cursor += 4;
            bool first_sample_flags_present = false;
            if ((flags & 0x000004U) != 0) {
                first_sample_flags = read_be32_or_zero(moof_bytes, cursor);
                first_sample_flags_present = true;
                cursor += 4;
            }
            std::uint64_t sample_decode_time = base_decode_time;
            for (std::uint32_t i = 0; i < sample_count && cursor <= moof_bytes.size(); ++i) {
                std::uint32_t sample_duration = default_sample_duration;
                if ((flags & 0x000100U) != 0 && cursor + 4 <= moof_bytes.size()) {
                    sample_duration = read_be32(moof_bytes, cursor);
                    cursor += 4;
                }

                if ((flags & 0x000200U) != 0) cursor += 4;  // skip sample_size
                std::uint32_t sample_flags = first_sample_flags_present && i == 0
                                                 ? first_sample_flags : default_sample_flags;
                if ((flags & 0x000400U) != 0 && cursor + 4 <= moof_bytes.size()) {
                    sample_flags = read_be32(moof_bytes, cursor);
                    cursor += 4;
                }
                std::int32_t composition_offset = 0;
                if ((flags & 0x000800U) != 0 && cursor + 4 <= moof_bytes.size()) {
                    composition_offset = static_cast<std::int32_t>(read_be32(moof_bytes, cursor));
                    cursor += 4;
                }
                const std::int64_t pt_signed =
                    static_cast<std::int64_t>(sample_decode_time) + static_cast<std::int64_t>(composition_offset);
                const std::uint64_t pt = pt_signed < 0 ? 0 : static_cast<std::uint64_t>(pt_signed);
                if (!earliest_presentation_time_set || pt < earliest_presentation_time) {
                    earliest_presentation_time = pt;
                    earliest_presentation_time_set = true;
                }
                if (i == 0 && (flags & 0x000400U) != 0) {
                    first_sample_flags = sample_flags;
                }
                duration += sample_duration;
                sample_decode_time += sample_duration;
            }
        }
    }

    std::uint8_t sap_type = 0;
    const bool first_sample_is_sync = (first_sample_flags & 0x00010000U) == 0;
    const bool is_video = (track_desc->handler_type == "vide");
    if (!is_video) {
        sap_type = 1;
    } else if (first_sample_is_sync) {
        sap_type = 2;
    }

    // A video keyframe: video track with sync first sample
    const bool is_video_keyframe = is_video && first_sample_is_sync;

    // Combine moof+mdat into a single owned payload (CMSF compliance).
    std::vector<std::uint8_t> payload;
    payload.reserve(moof_bytes.size() + mdat_bytes.size());
    payload.insert(payload.end(), moof_bytes.begin(), moof_bytes.end());
    payload.insert(payload.end(), mdat_bytes.begin(), mdat_bytes.end());

    return MediaFragment{
        .group_id = group_id,
        .object_id = 0,
        .track_name = track_name,
        .start_time_us = scale_to_us(base_decode_time, track_desc->timescale),
        .duration_us = scale_to_us(duration, track_desc->timescale),
        .earliest_presentation_time_us = scale_to_us(earliest_presentation_time, track_desc->timescale),
        .sap_type = sap_type,
        .has_sap_type = true,  // moqxr computed a concrete SAP type above
        .is_video_keyframe = is_video_keyframe,
        .payload = {.span = {}, .owned_bytes = std::move(payload)},
    };
}

}  // namespace openmoq::publisher
