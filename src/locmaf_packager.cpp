#include "locmaf_packager.h"

#include "openmoq/publisher/locmaf_encoder.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <utility>

namespace openmoq::publisher {
namespace {
PublishPlan package(const SegmentedMp4& segmented, const PublisherConfig& config) {
    return build_publish_plan(segmented, config.draft_version, config.include_sap,
                              config.include_msf_timeline, config.vod, config.drm_systems);
}

void preserve_chunk_boxes(SegmentedMp4& segmented, const ParsedMp4& parsed) {
    // Preserve the entire original chunk, including auxiliary boxes that the
    // ordinary sample splitter does not carry. CMAF's default path is unchanged.
    std::size_t prefix_start = 0;
    const Mp4Box* pending_moof = nullptr;
    for (const auto& box : parsed.top_level_boxes) {
        if (box.type == "ftyp" || box.type == "moov") {
            prefix_start = box.span.offset + box.span.size;
        } else if (box.type == "moof") {
            pending_moof = &box;
        } else if (box.type == "mdat" && pending_moof != nullptr) {
            for (auto& fragment : segmented.fragments) {
                if (fragment.payload.span.offset == pending_moof->span.offset &&
                    fragment.payload.owned_bytes.empty()) {
                    fragment.payload.span = {prefix_start, box.span.offset + box.span.size - prefix_start};
                    break;
                }
            }
            prefix_start = box.span.offset + box.span.size;
            pending_moof = nullptr;
        }
    }
}

std::vector<std::uint8_t> fragmented_init_box(const Mp4Box& box,
                                            std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> payload;
    if (box.type == "stsz") {
        payload.resize(12); // FullBox, default sample size, sample count.
    } else if (box.type == "stts" || box.type == "stsc" || box.type == "stco" ||
               box.type == "co64" || box.type == "ctts" || box.type == "stss") {
        payload.resize(8); // FullBox and empty entry table.
    } else if (!box.children.empty()) {
        for (const auto& child : box.children) {
            const auto encoded = fragmented_init_box(child, bytes);
            payload.insert(payload.end(), encoded.begin(), encoded.end());
        }
    } else {
        const auto original = slice_bytes(bytes, box.span);
        return {original.begin(), original.end()};
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() - 8) {
        throw std::runtime_error("LOCMAF initialization box is too large");
    }
    const auto size = static_cast<std::uint32_t>(payload.size() + 8);
    std::vector<std::uint8_t> result;
    result.reserve(size);
    for (int shift = 24; shift >= 0; shift -= 8) result.push_back(static_cast<std::uint8_t>(size >> shift));
    result.insert(result.end(), box.type.begin(), box.type.end());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

void normalize_progressive_init(SegmentedMp4& segmented) {
    // The source sample tables still describe offsets in the progressive file.
    // In a fragmented initialization those tables must be empty: trun now owns
    // the samples. Keep this correction local to the opt-in packaging path.
    const auto& original = segmented.initialization_segment.owned_bytes;
    const auto boxes = parse_mp4_boxes(original);
    std::vector<std::uint8_t> init;
    for (const auto& box : boxes) {
        const auto encoded = fragmented_init_box(box, original);
        init.insert(init.end(), encoded.begin(), encoded.end());
    }
    segmented.initialization_segment.owned_bytes = std::move(init);
}
}

PublishPlan prepare_locmaf_plan(const ParsedMp4& parsed, const PublisherConfig& config) {
    for (const auto& box : parsed.top_level_boxes) {
        if (box.type == "moof" &&
            std::count_if(box.children.begin(), box.children.end(),
                          [](const auto& child) { return child.type == "traf"; }) != 1) {
            throw std::runtime_error("LOCMAF requires demuxed single-traf chunks; use FFmpeg separate_moof");
        }
    }
    // Keep existing chunks: splitting encrypted/extended moofs into synthetic
    // single-sample moofs would discard information before LOCMAF can inspect it.
    auto segmented = segment_for_cmaf(parsed, CmafObjectMode::kCoalesced);
    if (find_first_box(parsed.top_level_boxes, "moof") == nullptr) {
        normalize_progressive_init(segmented);
    }
    preserve_chunk_boxes(segmented, parsed);
    const auto original = package(segmented, config);
    for (auto& track : segmented.tracks) {
        const auto init = std::find_if(original.track_initializations.begin(), original.track_initializations.end(),
            [&](const auto& entry) { return entry.track_name == track.track_name; });
        if (init == original.track_initializations.end()) continue;
        try {
            LocmafEncoder encoder(init->init_segment);
            struct Converted {
                std::size_t index;
                std::size_t group;
                std::size_t object;
                std::vector<std::uint8_t> payload;
            };
            std::vector<Converted> converted;
            std::size_t group = 0;
            std::size_t object = 0;
            for (std::size_t i = 0; i < segmented.fragments.size(); ++i) {
                const auto& fragment = segmented.fragments[i];
                if (fragment.track_name != track.track_name) continue;
                if (!converted.empty() && fragment.has_sap_type && fragment.sap_type != 0) {
                    ++group;
                    object = 0;
                }
                const auto bytes = fragment.payload.owned_bytes.empty()
                    ? slice_bytes(parsed.bytes, fragment.payload.span)
                    : std::span<const std::uint8_t>(fragment.payload.owned_bytes);
                // Full headers make every cached object usable by an arbitrary
                // FETCH/late subscriber without rewriting an existing object ID.
                converted.push_back({i, group, object, encoder.encode(bytes, group, object, true)});
                ++object;
            }
            // Commit only after the whole track passes preflight. An ineligible
            // later chunk must not leave a partly LOCMAF, partly CMAF track.
            for (auto& chunk : converted) {
                auto& fragment = segmented.fragments[chunk.index];
                fragment.group_id = chunk.group;
                fragment.object_id = chunk.object;
                fragment.payload.span = {};
                fragment.payload.owned_bytes = std::move(chunk.payload);
            }
            track.packaging = "locmaf";
        } catch (const LocmafIneligible& error) {
            std::cerr << "[locmaf] track " << track.track_name << " remains CMAF: " << error.what() << '\n';
        }
    }
    return package(segmented, config);
}
}
