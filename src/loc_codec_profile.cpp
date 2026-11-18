#include "openmoq/publisher/loc_codec_profile.h"

#include <array>

namespace openmoq::publisher {
const LocCodecProfile &avc_loc_profile();
const LocCodecProfile &aac_loc_profile();

namespace {
std::array<const LocCodecProfile *, 2> profiles() {
    return {&avc_loc_profile(), &aac_loc_profile()};
}
} // namespace

const LocCodecProfile *find_loc_profile(const TrackDescription &t) {
    for (const auto *profile : profiles())
        if (profile->matches(t))
            return profile;
    return nullptr;
}

const LocCodecProfile *find_loc_profile_for_codec(std::string_view codec, bool video) {
    for (const auto *profile : profiles())
        if (profile->matches_codec(codec, video))
            return profile;
    return nullptr;
}
} // namespace openmoq::publisher
