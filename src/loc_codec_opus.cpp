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
    } else {
        head.push_back(static_cast<std::uint8_t>(r.get(1)));
        head.push_back(static_cast<std::uint8_t>(r.get(1)));
        for (std::uint64_t i = 0; i < channels; ++i)
            head.push_back(static_cast<std::uint8_t>(r.get(1)));
    }
    r.end();
    return head;
}

bool valid_opus_head(std::span<const std::uint8_t> head) {
    static constexpr char kMagic[] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    if (head.size() < kOpusHeadFixedSize || !std::equal(head.begin(), head.begin() + 8, kMagic) || head[8] != 1 ||
        head[9] == 0)
        return false;
    const std::size_t expected = head[18] == 0 ? kOpusHeadFixedSize : kOpusHeadFixedSize + 2 + head[9];
    return head.size() == expected && (head[18] != 0 || head[9] <= 2);
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
