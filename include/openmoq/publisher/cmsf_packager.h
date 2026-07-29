#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/moq_draft.h"

namespace openmoq::publisher {

enum class CmsfObjectKind {
    kInitialization,
    kMetadata,
    kMedia,
};

struct CmsfObject {
    CmsfObjectKind kind = CmsfObjectKind::kInitialization;
    std::string track_name;
    std::size_t group_id = 0;
    std::uint64_t subgroup_id = 0;
    std::size_t object_id = 0;
    std::uint64_t media_time_us = 0;
    std::uint64_t media_duration_us = 0;
    std::uint8_t sap_type = 0;   // concrete CMSF SAP type 0..3 when has_sap_type
    bool has_sap_type = false;   // true: sap_type was computed (carried from MediaFragment)
    ByteSpan payload;
    std::vector<std::uint8_t> owned_payload;
};

struct TrackInitialization {
    std::string track_name;
    std::vector<std::uint8_t> codec_payload;
    std::vector<std::uint8_t> init_segment;
};

struct PublishPlan {
    DraftProfile draft;
    std::vector<TrackDescription> tracks;
    std::vector<TrackInitialization> track_initializations;
    std::vector<CmsfObject> objects;
};

// vod: when true the plan describes a Video-On-Demand asset (isLive false,
// trackDuration present, generatedAt omitted per MSF 5.1.2). Defaults to
// false because this publisher is live, or simulating live, unless
// explicitly configured otherwise.
PublishPlan build_publish_plan(const SegmentedMp4& segmented_mp4,
                               DraftVersion version,
                               bool include_sap = false,
                               bool include_msf_timeline = false,
                               bool vod = false);
std::string render_publish_plan(const PublishPlan& plan);
PublishPlan materialize_publish_plan(const PublishPlan& plan, std::span<const std::uint8_t> bytes);
void emit_plan_objects(const PublishPlan& plan,
                       std::span<const std::uint8_t> bytes,
                       const std::filesystem::path& output_dir);

// Build catalog JSON and track-specific init segments for live streaming
struct LiveCatalog {
    std::vector<std::uint8_t> catalog_payload;
    std::vector<TrackInitialization> track_initializations;
};
LiveCatalog build_live_catalog(const std::vector<TrackDescription>& tracks,
                               std::span<const std::uint8_t> init_segment,
                               bool is_live = true);

// Base64-encoded, track-specific CMAF init segment (ftyp + single-track moov)
// for the track at track_index within init_segment. This is exactly the value
// build_live_catalog embeds in each track's "initData" field, exposed so live
// sources that assemble catalogs from per-path init segments can reuse it.
std::string track_init_data_base64(std::span<const std::uint8_t> init_segment,
                                   const TrackDescription& track,
                                   std::size_t track_index);

}  // namespace openmoq::publisher
