#include "loc_codec_internal.h"

namespace openmoq::publisher {
namespace {
using loc_detail::Reader;
using loc_detail::fail;

// VPCodecConfigurationBox payload (a FullBox): version 1, flags 0, profile,
// level, bit depth/chroma/range, three colour fields and a 16-bit initialization
// data size that must be zero.
void validate_vpcc(std::span<const std::uint8_t> payload) {
    Reader r{payload};
    if (r.get(1) != 1 || r.get(3) != 0)
        fail("invalid VP9 configuration");
    r.skip(6);
    if (r.get(2) != 0)
        fail("VP9 codec initialization data unsupported");
    r.end();
}

class Vp9LocProfile final : public LocCodecProfile {
public:
    bool matches(const TrackDescription &t) const override {
        return t.handler_type == "vide" && t.sample_entry_type == "vp09";
    }
    bool matches_codec(std::string_view codec, bool video) const override {
        return video && codec.starts_with("vp09.");
    }
    // The WebCodecs VP9 registration says the description is not used, so a
    // validated vpcC yields no Video Config; the codec string carries profile,
    // level and bit depth.
    bool has_config() const override { return false; }
    std::vector<std::uint8_t> extract_config(const TrackDescription &t) const override {
        std::string type;
        auto r = loc_detail::config_box_reader(t, type);
        if (type != "vpcC")
            fail("invalid VP9 configuration");
        validate_vpcc(r.b);
        return {};
    }
    LocSampleCheck check_sample(const TrackDescription &t, std::span<const std::uint8_t> data, bool) const override {
        extract_config(t);
        if (data.empty())
            fail("empty VP9 frame");
        // Uncompressed header: frame_marker(2) profile_low(1) profile_high(1)
        // [reserved_zero(1) when profile is 3] show_existing_frame(1) frame_type(1).
        // Only the first frame of a superframe decides keyframe status.
        const auto first = data[0];
        if ((first >> 6) != 2)
            fail("invalid VP9 frame marker");
        const unsigned profile = (((first >> 4) & 1) << 1) | ((first >> 5) & 1);
        const unsigned show_existing_bit = profile == 3 ? 2 : 3;
        LocSampleCheck check;
        if ((first >> show_existing_bit) & 1)
            return check;
        check.independent = ((first >> (show_existing_bit - 1)) & 1) == 0;
        return check;
    }
    bool accepts_live_track(std::string_view, std::span<const std::uint8_t> init_data, bool) const override {
        return init_data.empty();
    }
};
} // namespace
const LocCodecProfile &vp9_loc_profile() {
    static const Vp9LocProfile profile;
    return profile;
}
} // namespace openmoq::publisher
