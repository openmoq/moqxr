#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::publisher {

// Media classification for a LiveTrack. kUnset marks a legacy "bare" track that
// carries only a name + opaque payloads -- such tracks cannot be published via
// the libmoq service tier (which requires real media metadata) and remain
// legacy-only (MoqtSession path).
enum class LiveMediaType {
    kUnset,
    kVideo,
    kAudio,
};

enum class LivePackaging {
    kRaw,
    kCmaf,
};

struct LiveTrack {
    std::string track_name;

    // Optional media metadata. Required for libmoq-backed publish_live_objects;
    // omitting it (kUnset) keeps a track legacy-only. Defaults preserve
    // source-compatibility of existing `{.track_name = ...}` initializers.
    LiveMediaType media_type = LiveMediaType::kUnset;
    LivePackaging packaging = LivePackaging::kRaw;
    std::string codec;
    std::vector<std::uint8_t> init_data;  // codec/decoder config (CMAF init, SPS/PPS, ASC, ...)
    std::uint64_t bitrate = 0;            // max bitrate (bits/s); 0 => libmoq default

    // Video.
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Audio.
    std::uint32_t sample_rate = 0;
    std::uint32_t channel_count = 0;
};

struct LiveObject {
    std::string track_name;
    std::size_t group_id = 0;
    std::uint64_t subgroup_id = 0;
    std::size_t object_id = 0;
    std::uint64_t media_time_us = 0;
    std::uint64_t media_duration_us = 0;
    std::vector<std::uint8_t> payload;
    bool subgroup_contains_group_largest = true;
    bool final_in_subgroup = true;
};

struct LiveObjectSource {
    std::vector<LiveTrack> tracks;
    std::function<std::optional<LiveObject>()> next_object;
};

}  // namespace openmoq::publisher
