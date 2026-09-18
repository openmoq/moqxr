#include "loc_packager.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

namespace openmoq::publisher {
namespace {
std::uint64_t microseconds(std::uint64_t ticks, std::uint32_t timescale) {
    if (timescale == 0 || ticks / timescale > UINT64_MAX / 1000000) {
        throw std::runtime_error("LOC timestamp cannot be represented in microseconds");
    }
    const auto whole = (ticks / timescale) * 1000000;
    const auto fraction = (ticks % timescale) * 1000000 / timescale;
    if (fraction > UINT64_MAX - whole) throw std::runtime_error("LOC timestamp overflow");
    return whole + fraction;
}
}

LocTrackEncoder::LocTrackEncoder(const TrackDescription& track)
    : track_(track), config_(loc_codec_config(track)) {
    if (track.timescale == 0) throw std::runtime_error("LOC requires a nonzero timescale");
    track_.packaging = "loc";
}

void LocTrackEncoder::require_random_access() {
    waiting_for_keyframe_ = true;
}

std::vector<MediaFragment> LocTrackEncoder::encode(std::span<const EncodedSample> samples) {
    std::vector<MediaFragment> output;
    const bool video = track_.handler_type == "vide";
    for (const auto& sample : samples) {
        if (sample.track_name != track_.track_name || sample.timescale != track_.timescale) {
            throw std::runtime_error("LOC sample does not match track configuration");
        }
        if (sample.presentation_time < 0 || sample.duration == 0 || sample.bytes.empty() ||
            sample.bytes.size() > 16 * 1024 * 1024) {
            throw std::runtime_error("LOC requires nonnegative PTS, nonempty bounded samples, and positive duration");
        }
        if (sample.duration > UINT64_MAX - sample.decode_time) {
            throw std::runtime_error("LOC decode timestamp overflow");
        }
        if (expected_decode_time_) {
            if (sample.decode_time < *expected_decode_time_) {
                throw std::runtime_error("LOC decode timeline overlaps or regresses");
            }
            if (video && sample.decode_time != *expected_decode_time_) require_random_access();
        }
        expected_decode_time_ = sample.decode_time + sample.duration;
        if (video && waiting_for_keyframe_ && !sample.independent) continue;
        const bool new_group = !started_ || !video || sample.independent;
        if (new_group) {
            if (started_) {
                if (group_ == std::numeric_limits<std::size_t>::max()) throw std::runtime_error("LOC group ID overflow");
                ++group_;
            }
            object_ = 0;
            started_ = true;
            waiting_for_keyframe_ = false;
        }
        MediaFragment fragment;
        fragment.track_name = sample.track_name;
        fragment.group_id = static_cast<std::size_t>(group_);
        fragment.object_id = static_cast<std::size_t>(object_);
        fragment.start_time_us = microseconds(sample.decode_time, sample.timescale);
        fragment.earliest_presentation_time_us = microseconds(static_cast<std::uint64_t>(sample.presentation_time), sample.timescale);
        fragment.duration_us = microseconds(sample.duration, sample.timescale);
        fragment.is_video_keyframe = video && sample.independent;
        fragment.has_sap_type = true;
        fragment.sap_type = sample.independent ? 1 : 0;
        fragment.payload.owned_bytes = sample.bytes;
        fragment.properties = {{8, static_cast<std::uint64_t>(sample.timescale)},
                               {16, static_cast<std::uint64_t>(sample.presentation_time)}};
        // One complete frame per object: S/E set, I only for independent frames.
        if (video) fragment.properties.push_back({9, std::vector<std::uint8_t>{static_cast<std::uint8_t>(sample.independent ? 0xe0 : 0xc0)}});
        // Repeating extradata keeps retained objects self-describing without
        // requiring a cache to rewrite an existing object or fetch prior state.
        fragment.properties.push_back({video ? 13U : 15U, config_});
        static_cast<void>(serialize_object_properties(fragment.properties));
        output.push_back(std::move(fragment));
        if (object_ == std::numeric_limits<std::size_t>::max()) throw std::runtime_error("LOC object ID overflow");
        ++object_;
    }
    return output;
}

PublishPlan prepare_loc_plan(const ParsedMp4& parsed, const PublisherConfig& config) {
    if (config.draft_version != DraftVersion::kDraft18) throw std::runtime_error("LOC-04 requires draft 18");
    const auto samples = read_encoded_samples(parsed);
    SegmentedMp4 segmented;
    segmented.tracks = parsed.tracks;
    for (const auto& box : parsed.top_level_boxes) {
        if (box.type != "ftyp" && box.type != "moov") continue;
        const auto bytes = slice_bytes(parsed.bytes, box.span);
        auto& init = segmented.initialization_segment.owned_bytes;
        init.insert(init.end(), bytes.begin(), bytes.end());
    }
    std::map<std::string, LocTrackEncoder> encoders;
    for (auto& track : segmented.tracks) {
        encoders.try_emplace(track.track_name, track);
        track.packaging = "loc";
    }
    for (const auto& sample : samples) {
        auto fragments = encoders.at(sample.track_name).encode(std::span<const EncodedSample>(&sample, 1));
        for (auto& fragment : fragments) segmented.fragments.push_back(std::move(fragment));
    }
    if (segmented.fragments.empty()) throw std::runtime_error("LOC input has no decodable samples");
    return build_publish_plan(segmented, config.draft_version, config.include_sap,
                              config.include_msf_timeline, config.vod, config.drm_systems);
}
} // namespace openmoq::publisher
