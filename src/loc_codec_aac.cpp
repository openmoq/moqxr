#include "loc_codec_internal.h"

namespace openmoq::publisher {
namespace {
using loc_detail::Reader;
using loc_detail::fail;
// MPEG-4 descriptors use a variable length field independent of MP4 box
// lengths.
std::span<const std::uint8_t> descriptor(Reader &r, unsigned tag) {
    if (r.get(1) != tag)
        fail("unexpected AAC descriptor");
    std::size_t size = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto v = r.get(1);
        size = (size << 7) | (v & 127);
        if (!(v & 128)) {
            const auto start = r.pos;
            r.skip(size);
            return r.b.subspan(start, size);
        }
    }
    fail("invalid AAC descriptor length");
}

class AacLocProfile final : public LocCodecProfile {
public:
    bool matches(const TrackDescription &t) const override {
        return t.handler_type == "soun" && t.sample_entry_type == "mp4a";
    }
    bool matches_codec(std::string_view codec, bool video) const override {
        return !video && codec.starts_with("mp4a.40.");
    }
    std::vector<std::uint8_t> extract_config(const TrackDescription &t) const override {
        std::string type;
        auto r = loc_detail::config_box_reader(t, type);
        if (type != "esds" || r.get(4))
            fail("invalid AAC configuration");
        Reader es{descriptor(r, 3)};
        es.skip(2);
        const auto flags = es.get(1);
        if (flags & 128)
            es.skip(2);
        if (flags & 64) {
            auto n = es.get(1);
            es.skip(n);
        }
        if (flags & 32)
            es.skip(2);
        Reader dec{descriptor(es, 4)};
        if (dec.get(1) != 0x40 || (dec.get(1) >> 2) != 5)
            fail("unsupported AAC object/stream type");
        dec.skip(11);
        const auto config = descriptor(dec, 5);
        if (config.size() < 2 || (config[0] >> 3) != 2 || (((config[0] & 7) << 1) | (config[1] >> 7)) >= 13 ||
            ((config[1] >> 3) & 15) == 0 || ((config[1] >> 3) & 15) > 7)
            fail("only AAC-LC configuration supported");
        return {config.begin(), config.end()};
    }
    LocSampleCheck check_sample(const TrackDescription &, std::span<const std::uint8_t>, bool) const override {
        LocSampleCheck check;
        check.independent = true;
        return check;
    }
    bool accepts_live_track(std::string_view, std::span<const std::uint8_t> init_data, bool) const override {
        return init_data.size() >= 2;
    }
};
} // namespace
const LocCodecProfile &aac_loc_profile() {
    static const AacLocProfile profile;
    return profile;
}
} // namespace openmoq::publisher
