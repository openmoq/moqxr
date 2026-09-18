#pragma once

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/encoded_sample.h"
#include "openmoq/publisher/publisher_api.h"

#include <optional>

namespace openmoq::publisher {

class LocTrackEncoder {
public:
    explicit LocTrackEncoder(const TrackDescription& track);
    std::vector<MediaFragment> encode(std::span<const EncodedSample> samples);
    // Retain object identity progression, but require a new independent frame.
    void require_random_access();
private:
    TrackDescription track_;
    std::vector<std::uint8_t> config_;
    std::uint64_t group_ = 0;
    std::uint64_t object_ = 0;
    bool started_ = false;
    bool waiting_for_keyframe_ = true;
    std::optional<std::uint64_t> expected_decode_time_;
};

PublishPlan prepare_loc_plan(const ParsedMp4& parsed, const PublisherConfig& config);

} // namespace openmoq::publisher
