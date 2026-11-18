#pragma once
#include "openmoq/publisher/mp4_box.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace openmoq::publisher {
struct LocSampleCheck {
    // Keyframe status read from the bitstream, not the container sync flag.
    bool independent = false;
    // Set only when the profile rewrote the sample (for example SRT stripping
    // parameter sets that are byte-identical to the codec configuration).
    std::optional<std::vector<std::uint8_t>> normalized;
};

// One profile per codec. A profile owns everything codec specific that LOC
// publishing needs: sample entry recognition, decoder configuration, keyframe
// detection and the explicit object API gate.
class LocCodecProfile {
public:
    virtual ~LocCodecProfile() = default;
    // Sample entry type plus handler type of a parsed track.
    virtual bool matches(const TrackDescription &) const = 0;
    // Codec string prefix plus media kind of an explicitly declared live track.
    virtual bool matches_codec(std::string_view codec, bool video) const = 0;
    // Validated decoder configuration carried as Video Config / Audio Config.
    virtual std::vector<std::uint8_t> extract_config(const TrackDescription &) const = 0;
    // False when the codec defines no extradata, so no config property is sent.
    virtual bool has_config() const { return true; }
    // Throws std::runtime_error for samples this profile does not publish.
    virtual LocSampleCheck check_sample(const TrackDescription &, std::span<const std::uint8_t> sample,
                                        bool strip_matching_config) const = 0;
    // Gate for explicit object sources: codec string and init data must agree.
    virtual bool accepts_live_track(std::string_view codec, std::span<const std::uint8_t> init_data,
                                    bool video) const = 0;
};

// nullptr when no profile supports the track or codec string.
const LocCodecProfile *find_loc_profile(const TrackDescription &);
const LocCodecProfile *find_loc_profile_for_codec(std::string_view codec, bool video);
} // namespace openmoq::publisher
