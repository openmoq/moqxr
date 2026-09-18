#pragma once
#include "openmoq/publisher/mp4_box.h"

namespace openmoq::publisher {
struct EncodedSample {
    std::string track_name;
    std::uint32_t timescale = 0;
    std::uint64_t decode_time = 0;
    std::int64_t presentation_time = 0;
    std::uint32_t duration = 0;
    bool independent = false;
    std::vector<std::uint8_t> bytes;
};
// Validate the full MP4 initialization before announcing a live LOC catalog.
// Requires no media data and applies the same preflight as batch extraction.
// Live callers set fragmented_only to reject initial progressive samples.
void validate_loc_init(const ParsedMp4 &, bool fragmented_only = false);
std::vector<EncodedSample> read_encoded_samples(const ParsedMp4 &);
// Live offsets must be relative to this moof; absolute file bases cannot be
// resolved. SRT demux may opt in to stripping byte-identical avcC SPS/PPS;
// changed configurations and layered NALs remain errors.
std::vector<EncodedSample> read_fragment_samples(std::span<const std::uint8_t> moof, std::span<const std::uint8_t> mdat,
                                                 std::span<const TrackDescription> tracks,
                                                 bool strip_matching_avc_config = false);
std::vector<std::uint8_t> loc_codec_config(const TrackDescription &);
} // namespace openmoq::publisher
