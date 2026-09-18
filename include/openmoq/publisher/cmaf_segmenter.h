#pragma once

#include <optional>
#include <string>
#include <vector>

#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/object_properties.h"

namespace openmoq::publisher {

enum class CmafObjectMode {
    kSplit,
    kCoalesced,
};

struct PayloadBuffer {
    ByteSpan span;
    std::vector<std::uint8_t> owned_bytes;
};

// Original elementary-stream clock, retained separately from synthesized CMAF.
// SRT preserves the original 33-bit 90 kHz PES PTS/DTS and the A/V offset;
// LOC rejects a backwards/wrapped clock rather than silently rebasing it.
// A zero duration means the source does not signal it (e.g. video PES).
// Sequence increments per track before queueing so consumers can detect drops.
struct SourceSampleTiming {
    std::uint64_t decode_time = 0;
    std::int64_t presentation_time = 0;
    std::uint64_t duration = 0;
    std::uint32_t timescale = 90000;
    std::uint64_t sequence = 0;
    // AAC AudioSpecificConfig from this ADTS frame, for configuration checks.
    std::vector<std::uint8_t> codec_config{};
};

struct MediaFragment {
    std::size_t group_id = 0;
    std::size_t object_id = 0;
    std::string track_name;
    std::uint64_t start_time_us = 0;
    std::uint64_t duration_us = 0;
    std::uint64_t earliest_presentation_time_us = 0;
    std::uint8_t sap_type = 0;       // concrete CMSF SAP type 0..3 when has_sap_type
    bool has_sap_type = false;       // true: sap_type was computed (vs unset/unknown)
    bool is_video_keyframe = false;  // True if this is a video track IDR/keyframe fragment
    std::uint64_t creation_time_us = 0;  // Wall-clock time when fragment was created (for queue delay measurement)
    PayloadBuffer payload;
    std::vector<ObjectProperty> properties{};
    std::optional<SourceSampleTiming> source_timing{};
};

struct SegmentedMp4 {
    PayloadBuffer initialization_segment;
    std::vector<MediaFragment> fragments;
    std::vector<TrackDescription> tracks;
};

SegmentedMp4 segment_for_cmaf(const ParsedMp4& parsed_mp4, CmafObjectMode object_mode = CmafObjectMode::kSplit);
std::string summarize_tracks(const std::vector<TrackDescription>& tracks);
std::size_t payload_size(const PayloadBuffer& payload);

// Build a MediaFragment from a single moof+mdat pair for live streaming.
// group_id is assigned by the caller (incremented per track).
// The fragment owns the combined moof+mdat bytes.
MediaFragment build_live_fragment(std::span<const std::uint8_t> moof_bytes,
                                  std::span<const std::uint8_t> mdat_bytes,
                                  const std::vector<TrackDescription>& tracks,
                                  std::size_t group_id);

}  // namespace openmoq::publisher
