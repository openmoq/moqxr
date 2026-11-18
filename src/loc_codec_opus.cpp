#include "loc_codec_internal.h"

#include <algorithm>

namespace openmoq::publisher {
namespace {
using loc_detail::Reader;
using loc_detail::fail;

constexpr std::size_t kOpusHeadFixedSize = 19;

void put_le16(std::vector<std::uint8_t> &out, std::uint64_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_le32(std::vector<std::uint8_t> &out, std::uint64_t v) {
    put_le16(out, v);
    put_le16(out, v >> 16);
}

// RFC 7845 section 5.1.1 channel mapping rules for families 1 and 255.
bool valid_channel_mapping(unsigned family, unsigned channels, unsigned streams, unsigned coupled,
                           std::span<const std::uint8_t> mapping) {
    if (channels == 0 || (family == 1 && channels > 8) || streams == 0 || coupled > streams ||
        streams + coupled > 255 || mapping.size() != channels)
        return false;
    return std::all_of(mapping.begin(), mapping.end(),
                       [&](std::uint8_t entry) { return entry == 255 || entry < streams + coupled; });
}

// ISO dOps (big endian, RFC 7845 fields) to the OpusHead identification header
// (little endian) that WebCodecs expects as the decoder description.
std::vector<std::uint8_t> opus_head_from_dops(std::span<const std::uint8_t> dops) {
    Reader r{dops};
    if (r.get(1) != 0)
        fail("unsupported Opus configuration version");
    const auto channels = r.get(1);
    const auto pre_skip = r.get(2);
    const auto rate = r.get(4);
    const auto gain = r.get(2);
    const auto family = r.get(1);
    if (channels == 0)
        fail("invalid Opus channel count");
    std::vector<std::uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1};
    head.push_back(static_cast<std::uint8_t>(channels));
    put_le16(head, pre_skip);
    put_le32(head, rate);
    put_le16(head, gain);
    head.push_back(static_cast<std::uint8_t>(family));
    if (family == 0) {
        if (channels > 2)
            fail("Opus mapping family 0 allows at most two channels");
    } else if (family == 1 || family == 255) {
        const auto streams = r.get(1);
        const auto coupled = r.get(1);
        std::vector<std::uint8_t> mapping;
        for (std::uint64_t i = 0; i < channels; ++i)
            mapping.push_back(static_cast<std::uint8_t>(r.get(1)));
        if (!valid_channel_mapping(static_cast<unsigned>(family), static_cast<unsigned>(channels),
                                   static_cast<unsigned>(streams), static_cast<unsigned>(coupled), mapping))
            fail("invalid Opus channel mapping");
        head.push_back(static_cast<std::uint8_t>(streams));
        head.push_back(static_cast<std::uint8_t>(coupled));
        head.insert(head.end(), mapping.begin(), mapping.end());
    } else {
        fail("unsupported Opus channel mapping family");
    }
    r.end();
    return head;
}

bool valid_opus_head(std::span<const std::uint8_t> head) {
    static constexpr char kMagic[] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    if (head.size() < kOpusHeadFixedSize || !std::equal(head.begin(), head.begin() + 8, kMagic) || head[8] != 1 ||
        head[9] == 0)
        return false;
    const unsigned channels = head[9], family = head[18];
    if (family == 0)
        return head.size() == kOpusHeadFixedSize && channels <= 2;
    if (family != 1 && family != 255)
        return false;
    return head.size() == kOpusHeadFixedSize + 2 + channels &&
           valid_channel_mapping(family, channels, head[19], head[20], head.subspan(21));
}

class OpusLocProfile final : public LocCodecProfile {
public:
    bool matches(const TrackDescription &t) const override {
        return t.handler_type == "soun" && (t.sample_entry_type == "Opus" || t.sample_entry_type == "opus");
    }
    bool matches_codec(std::string_view codec, bool video) const override { return !video && codec == "opus"; }
    std::vector<std::uint8_t> extract_config(const TrackDescription &t) const override {
        std::string type;
        auto r = loc_detail::config_box_reader(t, type);
        if (type != "dOps")
            fail("invalid Opus configuration");
        return opus_head_from_dops(r.b);
    }
    LocSampleCheck check_sample(const TrackDescription &, std::span<const std::uint8_t> sample, bool) const override {
        if (sample.empty())
            fail("empty Opus packet");
        LocSampleCheck check;
        check.independent = true;
        return check;
    }
    bool accepts_live_track(std::string_view, std::span<const std::uint8_t> init_data, bool) const override {
        return valid_opus_head(init_data);
    }
};
} // namespace
const LocCodecProfile &opus_loc_profile() {
    static const OpusLocProfile profile;
    return profile;
}
} // namespace openmoq::publisher
