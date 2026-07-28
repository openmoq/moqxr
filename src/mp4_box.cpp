#include "openmoq/publisher/mp4_box.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace openmoq::publisher {

namespace {

struct ChunkMapEntry {
    std::uint32_t first_chunk = 0;
    std::uint32_t samples_per_chunk = 0;
};

constexpr std::array<const char*, 11> kContainerBoxes = {
    "moov", "trak", "mdia", "minf", "stbl", "moof", "traf", "mvex", "edts", "dinf", "meta"};

bool is_container_type(std::string_view type) {
    for (const char* candidate : kContainerBoxes) {
        if (type == candidate) {
            return true;
        }
    }
    return false;
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_be64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | bytes[offset + i];
    }
    return value;
}

std::vector<Mp4Box> parse_box_range(std::span<const std::uint8_t> bytes,
                                    std::size_t begin,
                                    std::size_t end) {
    std::vector<Mp4Box> boxes;
    std::size_t cursor = begin;

    while (cursor + 8 <= end) {
        const std::uint32_t small_size = read_be32(bytes, cursor);
        const std::string type(reinterpret_cast<const char*>(bytes.data() + cursor + 4), 4);

        std::size_t header_size = 8;
        std::uint64_t box_size = small_size;

        if (small_size == 1) {
            if (cursor + 16 > end) {
                throw std::runtime_error("truncated extended-size MP4 box");
            }
            header_size = 16;
            box_size = read_be64(bytes, cursor + 8);
        } else if (small_size == 0) {
            box_size = end - cursor;
        }

        if (box_size < header_size || cursor + box_size > end) {
            throw std::runtime_error("invalid MP4 box size for box type " + type);
        }

        Mp4Box box{
            .type = type,
            .span = {.offset = cursor, .size = static_cast<std::size_t>(box_size)},
            .payload = {.offset = cursor + header_size, .size = static_cast<std::size_t>(box_size) - header_size},
            .children = {},
        };

        if (is_container_type(type)) {
            box.children = parse_box_range(bytes, box.payload.offset, box.payload.offset + box.payload.size);
        }

        boxes.push_back(std::move(box));
        cursor += static_cast<std::size_t>(box_size);
    }

    return boxes;
}

const Mp4Box* find_child(const Mp4Box& box, std::string_view type) {
    for (const auto& child : box.children) {
        if (child.type == type) {
            return &child;
        }
    }
    return nullptr;
}

const Mp4Box* find_track_box(const std::vector<Mp4Box>& top_level_boxes,
                             std::span<const std::uint8_t> bytes,
                             std::uint32_t track_id,
                             std::size_t track_index) {
    const Mp4Box* moov = find_first_box(top_level_boxes, "moov");
    if (moov == nullptr) {
        return nullptr;
    }

    std::size_t current_track_index = 0;
    for (const auto& child : moov->children) {
        if (child.type != "trak") {
            continue;
        }
        const Mp4Box* tkhd = find_child(child, "tkhd");
        std::uint32_t child_track_id = 0;
        if (tkhd != nullptr && tkhd->payload.size >= 20) {
            const std::uint8_t version = bytes[tkhd->payload.offset];
            const std::size_t track_id_offset = tkhd->payload.offset + (version == 1 ? 20 : 12);
            if (track_id_offset + 4 <= bytes.size()) {
                child_track_id = read_be32(bytes, track_id_offset);
            }
        }
        if ((track_id != 0 && child_track_id == track_id) || (track_id == 0 && current_track_index == track_index)) {
            return &child;
        }
        ++current_track_index;
    }
    return nullptr;
}

std::string codec_from_stsd(const Mp4Box& stsd, std::span<const std::uint8_t> bytes) {
    if (stsd.payload.size < 16) {
        return "unknown";
    }

    const std::size_t sample_entry_offset = stsd.payload.offset + 8;
    if (sample_entry_offset + 8 > bytes.size()) {
        return "unknown";
    }

    return std::string(reinterpret_cast<const char*>(bytes.data() + sample_entry_offset + 4), 4);
}

std::uint16_t read_be16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1]);
}

std::string hex_byte(std::uint8_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(value);
    return out.str();
}

std::string trim_trailing_zero_nibbles(std::uint32_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << value;
    std::string text = out.str();
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    return text;
}

std::string hevc_constraint_string(std::span<const std::uint8_t, 6> constraint_bytes) {
    std::size_t last_non_zero = constraint_bytes.size();
    while (last_non_zero > 0 && constraint_bytes[last_non_zero - 1] == 0) {
        --last_non_zero;
    }
    if (last_non_zero == 0) {
        return {};
    }

    std::ostringstream out;
    for (std::size_t index = 0; index < last_non_zero; ++index) {
        out << hex_byte(constraint_bytes[index]);
    }
    return out.str();
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
    for (std::uint32_t index = 0; index < sample_count && cursor + 4 <= bytes.size(); ++index) {
        sizes.push_back(read_be32(bytes, cursor));
        cursor += 4;
    }
    return sizes;
}

std::vector<std::uint64_t> parse_chunk_offsets(const Mp4Box& stbl, std::span<const std::uint8_t> bytes) {
    if (const Mp4Box* stco = find_child(stbl, "stco")) {
        const std::size_t table_offset = stco->payload.offset + 4;
        const std::uint32_t count = read_be32(bytes, table_offset);
        std::vector<std::uint64_t> offsets;
        offsets.reserve(count);
        std::size_t cursor = table_offset + 4;
        for (std::uint32_t index = 0; index < count && cursor + 4 <= bytes.size(); ++index) {
            offsets.push_back(read_be32(bytes, cursor));
            cursor += 4;
        }
        return offsets;
    }

    if (const Mp4Box* co64 = find_child(stbl, "co64")) {
        const std::size_t table_offset = co64->payload.offset + 4;
        const std::uint32_t count = read_be32(bytes, table_offset);
        std::vector<std::uint64_t> offsets;
        offsets.reserve(count);
        std::size_t cursor = table_offset + 4;
        for (std::uint32_t index = 0; index < count && cursor + 8 <= bytes.size(); ++index) {
            offsets.push_back(read_be64(bytes, cursor));
            cursor += 8;
        }
        return offsets;
    }

    return {};
}

std::vector<ChunkMapEntry> parse_chunk_map(const Mp4Box& stsc, std::span<const std::uint8_t> bytes) {
    const std::size_t table_offset = stsc.payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::vector<ChunkMapEntry> entries;
    entries.reserve(count);
    std::size_t cursor = table_offset + 4;
    for (std::uint32_t index = 0; index < count && cursor + 12 <= bytes.size(); ++index) {
        entries.push_back({
            .first_chunk = read_be32(bytes, cursor),
            .samples_per_chunk = read_be32(bytes, cursor + 4),
        });
        cursor += 12;
    }
    return entries;
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

    return sample_offsets;
}

bool hevc_sample_has_parameter_sets(std::span<const std::uint8_t> bytes, const ByteSpan& span) {
    std::size_t cursor = span.offset;
    const std::size_t end = span.offset + span.size;
    while (cursor + 6 <= end) {
        const std::uint32_t nal_size = read_be32(bytes, cursor);
        cursor += 4;
        if (nal_size < 2 || cursor + nal_size > end) {
            return false;
        }
        const std::uint8_t nal_unit_type = static_cast<std::uint8_t>((bytes[cursor] >> 1U) & 0x3FU);
        if (nal_unit_type == 32 || nal_unit_type == 33 || nal_unit_type == 34) {
            return true;
        }
        cursor += nal_size;
    }
    return false;
}

[[maybe_unused]] bool progressive_hevc_samples_are_hvc1_compatible(const Mp4Box& trak, std::span<const std::uint8_t> bytes) {
    const Mp4Box* mdia = find_child(trak, "mdia");
    const Mp4Box* minf = mdia == nullptr ? nullptr : find_child(*mdia, "minf");
    const Mp4Box* stbl = minf == nullptr ? nullptr : find_child(*minf, "stbl");
    const Mp4Box* stsz = stbl == nullptr ? nullptr : find_child(*stbl, "stsz");
    const Mp4Box* stsc = stbl == nullptr ? nullptr : find_child(*stbl, "stsc");
    if (stbl == nullptr || stsz == nullptr || stsc == nullptr) {
        return false;
    }

    const std::vector<std::uint32_t> sample_sizes = parse_sample_sizes(*stsz, bytes);
    const std::vector<std::uint64_t> chunk_offsets = parse_chunk_offsets(*stbl, bytes);
    const std::vector<ChunkMapEntry> chunk_map = parse_chunk_map(*stsc, bytes);
    if (sample_sizes.empty() || chunk_offsets.empty() || chunk_map.empty()) {
        return false;
    }

    const std::vector<std::uint64_t> sample_offsets = derive_sample_offsets(chunk_offsets, chunk_map, sample_sizes);
    if (sample_offsets.size() != sample_sizes.size()) {
        return false;
    }

    for (std::size_t index = 0; index < sample_sizes.size(); ++index) {
        const ByteSpan span{.offset = static_cast<std::size_t>(sample_offsets[index]), .size = sample_sizes[index]};
        if (span.offset + span.size > bytes.size()) {
            return false;
        }
        if (hevc_sample_has_parameter_sets(bytes, span)) {
            return false;
        }
    }
    return true;
}

bool fragmented_hevc_samples_are_hvc1_compatible(std::uint32_t track_id,
                                                 const std::vector<Mp4Box>& top_level_boxes,
                                                 std::span<const std::uint8_t> bytes) {
    const std::vector<const Mp4Box*> moofs = find_boxes(top_level_boxes, "moof");
    const std::vector<const Mp4Box*> mdats = find_boxes(top_level_boxes, "mdat");
    if (moofs.empty() || moofs.size() != mdats.size()) {
        return false;
    }

    for (std::size_t index = 0; index < moofs.size(); ++index) {
        const Mp4Box* traf = find_child(*moofs[index], "traf");
        const Mp4Box* tfhd = traf == nullptr ? nullptr : find_child(*traf, "tfhd");
        const Mp4Box* trun = traf == nullptr ? nullptr : find_child(*traf, "trun");
        if (traf == nullptr || tfhd == nullptr || trun == nullptr || tfhd->payload.size < 8) {
            return false;
        }

        if (read_be32(bytes, tfhd->payload.offset + 4) != track_id) {
            continue;
        }

        const std::uint32_t flags = (static_cast<std::uint32_t>(bytes[trun->payload.offset + 1]) << 16U) |
                                    (static_cast<std::uint32_t>(bytes[trun->payload.offset + 2]) << 8U) |
                                    static_cast<std::uint32_t>(bytes[trun->payload.offset + 3]);
        std::size_t cursor = trun->payload.offset + 4;
        if (cursor + 4 > bytes.size()) {
            return false;
        }
        const std::uint32_t sample_count = read_be32(bytes, cursor);
        cursor += 4;
        if ((flags & 0x000001U) != 0) {
            cursor += 4;
        }
        if ((flags & 0x000004U) != 0) {
            cursor += 4;
        }

        std::size_t payload_offset = mdats[index]->payload.offset;
        for (std::uint32_t sample_index = 0; sample_index < sample_count && cursor <= bytes.size(); ++sample_index) {
            if ((flags & 0x000100U) != 0) {
                cursor += 4;
            }

            std::uint32_t sample_size = 0;
            if ((flags & 0x000200U) != 0 && cursor + 4 <= bytes.size()) {
                sample_size = read_be32(bytes, cursor);
                cursor += 4;
            } else {
                return false;
            }

            if ((flags & 0x000400U) != 0) {
                cursor += 4;
            }
            if ((flags & 0x000800U) != 0) {
                cursor += 4;
            }

            const ByteSpan span{.offset = payload_offset, .size = sample_size};
            if (span.offset + span.size > bytes.size()) {
                return false;
            }
            if (hevc_sample_has_parameter_sets(bytes, span)) {
                return false;
            }
            payload_offset += sample_size;
        }
    }
    return true;
}

bool hevc_track_is_hvc1_compatible(const std::vector<Mp4Box>& top_level_boxes,
                                   std::span<const std::uint8_t> bytes,
                                   std::uint32_t track_id,
                                   std::size_t track_index) {
    const Mp4Box* trak = find_track_box(top_level_boxes, bytes, track_id, track_index);
    if (trak == nullptr) {
        return false;
    }
    if (find_boxes(top_level_boxes, "moof").empty()) {
        return false;
    }
    return fragmented_hevc_samples_are_hvc1_compatible(track_id, top_level_boxes, bytes);
}

std::size_t find_child_box_offset(const Mp4Box& sample_entry,
                                  std::span<const std::uint8_t> bytes,
                                  std::size_t child_offset,
                                  std::string_view type) {
    // sample_entry.span.size comes from an unchecked 32-bit box-size field in
    // the stsd entry (extract_tracks), so it cannot be trusted to bound the
    // scan: a fabricated huge value must not drive reads past the end of
    // `bytes`. Clamp to the real buffer, and clamp defensively against
    // size_t overflow in the offset+size sum itself (and in offset+child_offset).
    std::size_t entry_end = sample_entry.span.offset + sample_entry.span.size;
    if (entry_end < sample_entry.span.offset) {
        entry_end = bytes.size();
    }
    const std::size_t limit = std::min(entry_end, bytes.size());

    const std::size_t start = sample_entry.span.offset + child_offset;
    if (start < sample_entry.span.offset) {
        return 0;
    }

    for (std::size_t cursor = start; cursor + 8 <= limit; ++cursor) {
        const std::uint32_t box_size = read_be32(bytes, cursor);
        if (box_size < 8 || cursor + box_size > limit) {
            continue;
        }
        if (std::string_view(reinterpret_cast<const char*>(bytes.data() + cursor + 4), 4) == type) {
            return cursor;
        }
    }
    return 0;
}

bool decode_descriptor_length(std::span<const std::uint8_t> bytes,
                              std::size_t offset,
                              std::size_t limit,
                              std::size_t& length,
                              std::size_t& bytes_consumed) {
    length = 0;
    bytes_consumed = 0;
    while (offset + bytes_consumed < limit && bytes_consumed < 4) {
        const std::uint8_t value = bytes[offset + bytes_consumed];
        length = (length << 7U) | static_cast<std::size_t>(value & 0x7FU);
        ++bytes_consumed;
        if ((value & 0x80U) == 0) {
            return true;
        }
    }
    return false;
}

std::string avc_codec_string(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    const std::size_t avcc_offset = find_child_box_offset(sample_entry, bytes, 8 + 70, "avcC");
    if (avcc_offset == 0 || avcc_offset + 12 > bytes.size()) {
        return "avc1";
    }

    const std::uint8_t profile = bytes[avcc_offset + 9];
    const std::uint8_t compatibility = bytes[avcc_offset + 10];
    const std::uint8_t level = bytes[avcc_offset + 11];
    return sample_entry.type + "." + hex_byte(profile) + hex_byte(compatibility) + hex_byte(level);
}

std::string hevc_codec_string(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    const std::size_t hvcc_offset = find_child_box_offset(sample_entry, bytes, 8 + 70, "hvcC");
    if (hvcc_offset == 0 || hvcc_offset + 21 > bytes.size()) {
        return sample_entry.type;
    }

    const std::uint8_t profile_byte = bytes[hvcc_offset + 9];
    const char profile_space = (profile_byte >> 6U) == 1 ? 'A' : (profile_byte >> 6U) == 2 ? 'B' : (profile_byte >> 6U) == 3 ? 'C' : '\0';
    const std::uint8_t profile_idc = profile_byte & 0x1FU;
    const std::uint32_t compatibility_flags = read_be32(bytes, hvcc_offset + 10);
    const std::uint8_t level_idc = bytes[hvcc_offset + 20];
    const std::array<std::uint8_t, 6> constraint_bytes = {
        bytes[hvcc_offset + 14],
        bytes[hvcc_offset + 15],
        bytes[hvcc_offset + 16],
        bytes[hvcc_offset + 17],
        bytes[hvcc_offset + 18],
        bytes[hvcc_offset + 19],
    };

    std::ostringstream out;
    out << sample_entry.type << '.';
    if (profile_space != '\0') {
        out << profile_space;
    }
    out << static_cast<unsigned int>(profile_idc) << '.'
        << trim_trailing_zero_nibbles(compatibility_flags) << '.'
        << ((profile_byte & 0x20U) != 0 ? 'H' : 'L') << static_cast<unsigned int>(level_idc);
    const std::string constraint_string = hevc_constraint_string(constraint_bytes);
    if (!constraint_string.empty()) {
        out << '.' << constraint_string;
    }
    return out.str();
}

std::string mpeg4_audio_codec_string(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    const std::size_t esds_offset = find_child_box_offset(sample_entry, bytes, 8 + 28, "esds");
    if (esds_offset == 0 || esds_offset + 16 > bytes.size()) {
        return "mp4a.40.2";
    }

    std::uint8_t audio_object_type = 2;
    for (std::size_t cursor = esds_offset + 12; cursor + 2 <= sample_entry.span.offset + sample_entry.span.size; ++cursor) {
        if (bytes[cursor] != 0x05) {
            continue;
        }
        std::size_t length = 0;
        std::size_t length_bytes = 0;
        if (!decode_descriptor_length(bytes, cursor + 1, sample_entry.span.offset + sample_entry.span.size, length, length_bytes) ||
            length == 0) {
            continue;
        }
        const std::size_t config_offset = cursor + 1 + length_bytes;
        if (config_offset + length > sample_entry.span.offset + sample_entry.span.size) {
            continue;
        }
        const std::uint8_t config = bytes[config_offset];
        audio_object_type = static_cast<std::uint8_t>((config >> 3U) & 0x1FU);
        if (audio_object_type == 31 && length >= 2) {
            audio_object_type =
                static_cast<std::uint8_t>(32 + ((config & 0x07U) << 3U) + ((bytes[config_offset + 1] >> 5U) & 0x07U));
        }
        break;
    }

    std::ostringstream out;
    out << "mp4a.40." << static_cast<unsigned int>(audio_object_type);
    return out.str();
}

std::string codec_string_from_sample_entry(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    if (sample_entry.type == "avc1" || sample_entry.type == "avc3") {
        return avc_codec_string(sample_entry, bytes);
    }
    if (sample_entry.type == "hvc1" || sample_entry.type == "hev1") {
        return hevc_codec_string(sample_entry, bytes);
    }
    if (sample_entry.type == "mp4a") {
        return mpeg4_audio_codec_string(sample_entry, bytes);
    }
    if (sample_entry.type == "Opus" || sample_entry.type == "opus") {
        return "opus";
    }
    return sample_entry.type;
}

double frame_rate_from_stts(const Mp4Box* stts, std::uint32_t timescale, std::span<const std::uint8_t> bytes) {
    if (stts == nullptr || timescale == 0 || stts->payload.size < 8) {
        return 0.0;
    }

    const std::size_t table_offset = stts->payload.offset + 4;
    const std::uint32_t entry_count = read_be32(bytes, table_offset);
    std::size_t cursor = table_offset + 4;
    std::uint64_t sample_count = 0;
    std::uint64_t duration_sum = 0;
    for (std::uint32_t index = 0; index < entry_count && cursor + 8 <= bytes.size(); ++index) {
        const std::uint32_t run_count = read_be32(bytes, cursor);
        const std::uint32_t delta = read_be32(bytes, cursor + 4);
        sample_count += run_count;
        duration_sum += static_cast<std::uint64_t>(run_count) * delta;
        cursor += 8;
    }

    if (sample_count == 0 || duration_sum == 0) {
        return 0.0;
    }
    return static_cast<double>(sample_count) * static_cast<double>(timescale) / static_cast<double>(duration_sum);
}

// ISO 14496-12 BitRateBox inside a sample entry: bufferSizeDB, maxBitrate,
// avgBitrate, each 32-bit big-endian.
struct BitrateInfo {
    std::uint64_t max_bitrate = 0;
    std::uint64_t avg_bitrate = 0;
};

BitrateInfo bitrate_from_sample_entry(const Mp4Box& sample_entry,
                                      std::span<const std::uint8_t> bytes,
                                      std::size_t child_offset) {
    const std::size_t btrt_offset = find_child_box_offset(sample_entry, bytes, child_offset, "btrt");
    if (btrt_offset == 0 || btrt_offset + 20 > bytes.size()) {
        return {};
    }
    return BitrateInfo{
        .max_bitrate = read_be32(bytes, btrt_offset + 12),
        .avg_bitrate = read_be32(bytes, btrt_offset + 16),
    };
}

// MSF section 5.2.32 requires "standard Tags for Identifying Languages as
// defined by [LANG]", which resolves to BCP 47 / RFC 5646; section 2.2.1 of
// RFC 5646 requires the SHORTEST ISO 639 code ("en", not "eng"). mdhd stores
// ISO 639-2/T, so map the common codes to their ISO 639-1 two-letter form,
// covering both the bibliographic and terminological 639-2 spellings where
// they differ (e.g. "fre"/"fra" both map to "fr"). A language with no
// two-letter code (e.g. "haw", "mis") passes through unchanged, which is
// also spec-conformant BCP 47 usage.
std::string bcp47_from_iso639_2(const std::string& code) {
    static const std::vector<std::pair<std::string_view, std::string_view>> kToTwoLetter = {
        {"eng", "en"}, {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"}, {"ger", "de"},
        {"spa", "es"}, {"ita", "it"}, {"por", "pt"}, {"rus", "ru"}, {"jpn", "ja"},
        {"kor", "ko"}, {"zho", "zh"}, {"chi", "zh"}, {"nld", "nl"}, {"dut", "nl"},
        {"swe", "sv"}, {"nor", "no"}, {"dan", "da"}, {"fin", "fi"}, {"pol", "pl"},
        {"tur", "tr"}, {"ara", "ar"}, {"hin", "hi"}, {"tha", "th"}, {"vie", "vi"},
        {"ell", "el"}, {"gre", "el"}, {"heb", "he"}, {"ces", "cs"}, {"cze", "cs"},
        {"hun", "hu"}, {"ron", "ro"}, {"rum", "ro"}, {"ukr", "uk"}, {"ind", "id"},
    };
    for (const auto& [three, two] : kToTwoLetter) {
        if (code == three) {
            return std::string(two);
        }
    }
    return code;
}

// mdhd language is three 5-bit values, each offset by 0x60, packed into 16
// bits. Returns an empty string for the "und" (undetermined) code, and maps
// the decoded ISO 639-2/T code to its shortest BCP 47 form.
std::string language_from_mdhd(const Mp4Box& mdhd, std::span<const std::uint8_t> bytes) {
    if (mdhd.payload.size < 4) {
        return {};
    }
    const std::uint8_t version = bytes[mdhd.payload.offset];
    // v0: version+flags(4) + created(4) + modified(4) + timescale(4) + duration(4)
    // v1: version+flags(4) + created(8) + modified(8) + timescale(4) + duration(8)
    const std::size_t language_offset = mdhd.payload.offset + (version == 1 ? 32 : 20);
    if (language_offset + 2 > bytes.size()) {
        return {};
    }
    const std::uint16_t packed =
        static_cast<std::uint16_t>((bytes[language_offset] << 8U) | bytes[language_offset + 1]);
    std::string language;
    for (int shift = 10; shift >= 0; shift -= 5) {
        const auto code = static_cast<char>(((packed >> shift) & 0x1FU) + 0x60);
        if (code < 'a' || code > 'z') {
            return {};
        }
        language.push_back(code);
    }
    return language == "und" ? std::string{} : bcp47_from_iso639_2(language);
}

std::uint64_t duration_ms_from_mdhd(const Mp4Box& mdhd,
                                    std::uint32_t timescale,
                                    std::span<const std::uint8_t> bytes) {
    if (timescale == 0 || mdhd.payload.size < 4) {
        return 0;
    }
    const std::uint8_t version = bytes[mdhd.payload.offset];
    const std::size_t duration_offset = mdhd.payload.offset + (version == 1 ? 24 : 16);
    std::uint64_t duration = 0;
    if (version == 1) {
        if (duration_offset + 8 > bytes.size()) {
            return 0;
        }
        duration = (static_cast<std::uint64_t>(read_be32(bytes, duration_offset)) << 32U) |
                   read_be32(bytes, duration_offset + 4);
    } else {
        if (duration_offset + 4 > bytes.size()) {
            return 0;
        }
        duration = read_be32(bytes, duration_offset);
    }
    // duration * 1000 can overflow uint64_t for a pathological version-1
    // duration (up to 2^64-1); bail out to the "unknown" sentinel rather than
    // silently wrapping and reporting a bogus duration.
    constexpr std::uint64_t kMaxDurationForMsConversion = std::numeric_limits<std::uint64_t>::max() / 1000ULL;
    if (duration > kMaxDurationForMsConversion) {
        return 0;
    }
    return duration * 1000ULL / timescale;
}

}  // namespace

ParsedMp4 parse_mp4_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open MP4 file: " + path);
    }

    return parse_mp4_stream(input, path);
}

ParsedMp4 parse_mp4_stream(std::istream& input, std::string_view source_name) {
    ParsedMp4 parsed;

    input.seekg(0, std::ios::end);
    if (input.good()) {
        const auto end = input.tellg();
        if (end >= 0) {
            parsed.bytes.resize(static_cast<std::size_t>(end));
            input.seekg(0, std::ios::beg);
            input.read(reinterpret_cast<char*>(parsed.bytes.data()), static_cast<std::streamsize>(parsed.bytes.size()));
            if (!input) {
                throw std::runtime_error("failed to read MP4 input: " + std::string(source_name));
            }
        }
    }

    if (parsed.bytes.empty()) {
        input.clear();
        input.seekg(0, std::ios::beg);

        constexpr std::size_t kChunkSize = 16 * 1024;
        std::array<char, kChunkSize> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = input.gcount();
            if (bytes_read > 0) {
                parsed.bytes.insert(parsed.bytes.end(),
                                    reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                    reinterpret_cast<const std::uint8_t*>(buffer.data()) + bytes_read);
            }
        }
        if (!input.eof()) {
            throw std::runtime_error("failed to read MP4 input: " + std::string(source_name));
        }
    }

    parsed.top_level_boxes = parse_mp4_boxes(parsed.bytes);
    parsed.tracks = extract_tracks(parsed.top_level_boxes, parsed.bytes);
    return parsed;
}

std::vector<Mp4Box> parse_mp4_boxes(std::span<const std::uint8_t> bytes) {
    return parse_box_range(bytes, 0, bytes.size());
}

std::vector<TrackDescription> extract_tracks(const std::vector<Mp4Box>& top_level_boxes,
                                             std::span<const std::uint8_t> bytes) {
    std::vector<TrackDescription> tracks;
    const Mp4Box* moov = find_first_box(top_level_boxes, "moov");
    if (moov == nullptr) {
        return tracks;
    }

    std::size_t track_index = 0;
    for (const auto& trak : moov->children) {
        if (trak.type != "trak") {
            continue;
        }

        const Mp4Box* mdia = find_child(trak, "mdia");
        if (mdia == nullptr) {
            continue;
        }

        const Mp4Box* hdlr = find_child(*mdia, "hdlr");
        const Mp4Box* mdhd = find_child(*mdia, "mdhd");
        const Mp4Box* minf = find_child(*mdia, "minf");
        const Mp4Box* tkhd = find_child(trak, "tkhd");
        if (hdlr == nullptr || minf == nullptr) {
            continue;
        }

        const Mp4Box* stbl = find_child(*minf, "stbl");
        const Mp4Box* stsd = stbl == nullptr ? nullptr : find_child(*stbl, "stsd");
        if (stsd == nullptr || hdlr->payload.size < 12) {
            continue;
        }

        const std::size_t handler_offset = hdlr->payload.offset + 8;
        const std::string handler_type(reinterpret_cast<const char*>(bytes.data() + handler_offset), 4);
        const std::string source_sample_entry_type = codec_from_stsd(*stsd, bytes);
        std::string sample_entry_type = source_sample_entry_type;
        const Mp4Box sample_entry{
            .type = sample_entry_type,
            .span = {.offset = stsd->payload.offset + 8, .size = read_be32(bytes, stsd->payload.offset + 8)},
            .payload = {.offset = stsd->payload.offset + 16, .size = read_be32(bytes, stsd->payload.offset + 8) - 8},
            .children = {},
        };
        std::uint32_t track_id = 0;
        std::uint32_t timescale = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t channel_count = 0;
        std::uint32_t sample_rate = 0;
        double frame_rate = 0.0;
        if (tkhd != nullptr && tkhd->payload.size >= 20) {
            const std::uint8_t version = bytes[tkhd->payload.offset];
            const std::size_t track_id_offset = tkhd->payload.offset + (version == 1 ? 20 : 12);
            if (track_id_offset + 4 <= bytes.size()) {
                track_id = read_be32(bytes, track_id_offset);
            }
        }
        if (mdhd != nullptr && mdhd->payload.size >= 20) {
            const std::uint8_t version = bytes[mdhd->payload.offset];
            const std::size_t timescale_offset = mdhd->payload.offset + (version == 1 ? 20 : 12);
            if (timescale_offset + 4 <= bytes.size()) {
                timescale = read_be32(bytes, timescale_offset);
            }
        }
        if (source_sample_entry_type == "hev1" && hevc_track_is_hvc1_compatible(top_level_boxes, bytes, track_id, track_index)) {
            sample_entry_type = "hvc1";
        }
        const Mp4Box effective_sample_entry{
            .type = sample_entry_type,
            .span = sample_entry.span,
            .payload = sample_entry.payload,
            .children = {},
        };
        const std::string codec = codec_string_from_sample_entry(effective_sample_entry, bytes);
        if ((sample_entry_type == "avc1" || sample_entry_type == "avc3" || sample_entry_type == "hvc1" ||
             sample_entry_type == "hev1") &&
            sample_entry.payload.offset + 28 <= bytes.size()) {
            width = read_be16(bytes, sample_entry.payload.offset + 24);
            height = read_be16(bytes, sample_entry.payload.offset + 26);
            frame_rate = frame_rate_from_stts(find_child(*stbl, "stts"), timescale, bytes);
        }
        if ((sample_entry_type == "mp4a" || sample_entry_type == "Opus" || sample_entry_type == "opus") &&
            sample_entry.payload.offset + 28 <= bytes.size()) {
            channel_count = read_be16(bytes, sample_entry.payload.offset + 16);
            sample_rate = read_be16(bytes, sample_entry.payload.offset + 24);
        }

        // MSF 5.2.22 requires bitrate; ISO 14496-12 puts it in an optional
        // btrt box inside the sample entry. Same header offsets
        // build_track_codec_init_data uses: 8 + 70 past a VisualSampleEntry,
        // 8 + 28 past an AudioSampleEntry.
        const std::size_t sample_entry_child_offset = handler_type == "vide" ? 8 + 70 : 8 + 28;
        const BitrateInfo bitrate = bitrate_from_sample_entry(sample_entry, bytes, sample_entry_child_offset);

        std::string language;
        std::uint64_t duration_ms = 0;
        if (mdhd != nullptr) {
            language = language_from_mdhd(*mdhd, bytes);
            duration_ms = duration_ms_from_mdhd(*mdhd, timescale, bytes);
        }

        tracks.push_back({
            .track_id = track_id,
            .handler_type = handler_type,
            .codec = codec,
            .sample_entry_type = sample_entry_type,
            .track_name = handler_type + "_" + std::to_string(tracks.size() + 1),
            .packaging = "cmaf",
            .event_type = {},
            .mime_type = {},
            .depends = {},
            .timescale = timescale,
            .width = width,
            .height = height,
            .channel_count = channel_count,
            .sample_rate = sample_rate,
            .frame_rate = frame_rate,
            .max_bitrate = bitrate.max_bitrate,
            .avg_bitrate = bitrate.avg_bitrate,
            .duration_ms = duration_ms,
            .language = language,
        });
        ++track_index;
    }

    return tracks;
}

const Mp4Box* find_first_box(const std::vector<Mp4Box>& boxes, std::string_view type) {
    for (const auto& box : boxes) {
        if (box.type == type) {
            return &box;
        }
    }
    return nullptr;
}

std::vector<const Mp4Box*> find_boxes(const std::vector<Mp4Box>& boxes, std::string_view type) {
    std::vector<const Mp4Box*> matches;
    for (const auto& box : boxes) {
        if (box.type == type) {
            matches.push_back(&box);
        }
    }
    return matches;
}

const Mp4Box* find_child_box(const Mp4Box& box, std::string_view type) {
    return find_child(box, type);
}

std::span<const std::uint8_t> slice_bytes(std::span<const std::uint8_t> bytes, const ByteSpan& span) {
    return bytes.subspan(span.offset, span.size);
}

// --- StreamingMp4Reader ---

void StreamingMp4Reader::append(const std::uint8_t* data, std::size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
}

std::size_t StreamingMp4Reader::read_from(std::istream& input, std::size_t chunk_size) {
    const std::size_t old_size = buffer_.size();
    buffer_.resize(old_size + chunk_size);
    input.read(reinterpret_cast<char*>(buffer_.data() + old_size),
               static_cast<std::streamsize>(chunk_size));
    const auto bytes_read = static_cast<std::size_t>(input.gcount());
    buffer_.resize(old_size + bytes_read);
    return bytes_read;
}

std::optional<StreamingBoxResult> StreamingMp4Reader::next_box() {
    const std::size_t avail = buffer_.size() - consumed_;
    if (avail < 8) {
        return std::nullopt;
    }

    const std::uint8_t* p = buffer_.data() + consumed_;

    const std::uint32_t small_size =
        (static_cast<std::uint32_t>(p[0]) << 24U) |
        (static_cast<std::uint32_t>(p[1]) << 16U) |
        (static_cast<std::uint32_t>(p[2]) << 8U) |
        static_cast<std::uint32_t>(p[3]);

    std::string type(reinterpret_cast<const char*>(p + 4), 4);

    std::uint64_t box_size = small_size;
    if (small_size == 1) {
        if (avail < 16) {
            return std::nullopt;
        }
        box_size = 0;
        for (int i = 0; i < 8; ++i) {
            box_size = (box_size << 8U) | p[8 + i];
        }
        if (box_size < 16) {
            throw std::runtime_error("impossible extended box size");
        }
    } else if (small_size == 0) {
        // size==0 means "runs to EOF"; can't handle incrementally unless
        // we have all remaining data. Return nullopt to wait for more data.
        return std::nullopt;
    } else if (small_size < 8) {
        throw std::runtime_error("impossible MP4 box size");
    }

    if (avail < box_size) {
        return std::nullopt;
    }

    StreamingBoxResult result;
    result.type = std::move(type);
    result.bytes.assign(p, p + static_cast<std::size_t>(box_size));
    consumed_ += static_cast<std::size_t>(box_size);

    if (consumed_ > 64 * 1024) {
        compact();
    }

    return result;
}

void StreamingMp4Reader::compact() {
    if (consumed_ == 0) {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
    consumed_ = 0;
}

}  // namespace openmoq::publisher
