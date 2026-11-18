#include "loc_codec_internal.h"

#include <algorithm>

namespace openmoq::publisher {
namespace {
using loc_detail::Reader;
using loc_detail::fail;
// Read only first_mb_in_slice, needed to reject multiple coded pictures in a
// single MP4 sample. This is not a full H.264 slice-header/decoder validator.
std::uint32_t first_macroblock(std::span<const std::uint8_t> ebsp) {
    std::size_t cursor = 0;
    unsigned zeros = 0, bits_left = 0;
    std::uint8_t current = 0;
    auto bit = [&]() -> unsigned {
        if (!bits_left) {
            if (cursor == ebsp.size())
                fail("truncated AVC slice header");
            current = ebsp[cursor++];
            if (zeros >= 2 && current == 3) {
                if (cursor == ebsp.size() || ebsp[cursor] > 3)
                    fail("invalid AVC emulation prevention");
                current = ebsp[cursor++];
                zeros = 0;
            }
            zeros = current == 0 ? zeros + 1 : 0;
            bits_left = 8;
        }
        return (current >> --bits_left) & 1;
    };
    unsigned prefix = 0;
    while (!bit()) {
        if (++prefix > 31)
            fail("AVC first macroblock overflow");
    }
    std::uint32_t value = 1;
    for (unsigned i = 0; i < prefix; ++i)
        value = (value << 1) | bit();
    return value - 1;
}

class AvcLocProfile final : public LocCodecProfile {
public:
    bool matches(const TrackDescription &t) const override {
        return t.handler_type == "vide" && t.sample_entry_type == "avc1";
    }
    bool matches_codec(std::string_view codec, bool video) const override {
        return video && codec.starts_with("avc1.");
    }
    std::vector<std::uint8_t> extract_config(const TrackDescription &t) const override {
        std::string type;
        auto r = loc_detail::config_box_reader(t, type);
        if (type != "avcC" || r.b.size() < 7 || r.b[0] != 1 || (r.b[4] & 3) == 2)
            fail("invalid AVC configuration");
        r.skip(5);
        const auto sps_count = r.get(1) & 31;
        auto parameter_sets = [&](std::uint64_t count, unsigned type) {
            if (!count)
                fail("missing AVC parameter sets");
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto size = r.get(2);
                if (!size || size > r.b.size() - r.pos || (r.b[r.pos] & 31) != type)
                    fail("invalid AVC parameter set");
                r.skip(static_cast<std::size_t>(size));
            }
        };
        parameter_sets(sps_count, 7);
        parameter_sets(r.get(1), 8);
        // High profile records may append chroma/bit-depth and SPS extensions.
        if (r.pos < r.b.size()) {
            const auto profile = r.b[1];
            if (profile != 100 && profile != 110 && profile != 122 && profile != 144)
                fail("unexpected AVC configuration extension");
            r.skip(3);
            const auto count = r.get(1);
            if (count)
                parameter_sets(count, 13);
        }
        r.end();
        return {r.b.begin(), r.b.end()};
    }
    LocSampleCheck check_sample(const TrackDescription &t, std::span<const std::uint8_t> data,
                                bool strip_matching_config) const override {
        std::vector<std::uint8_t> normalized;
        const auto config = extract_config(t);
        const std::size_t width = (config[4] & 3) + 1;
        std::vector<std::span<const std::uint8_t>> parameter_sets;
        if (strip_matching_config) {
            Reader sets{config};
            sets.skip(5);
            const auto sps_count = sets.get(1) & 31;
            auto read_sets = [&](std::uint64_t count) {
                for (std::uint64_t i = 0; i < count; ++i) {
                    const auto length = sets.get(2);
                    const auto start = sets.pos;
                    sets.skip(static_cast<std::size_t>(length));
                    parameter_sets.push_back(sets.b.subspan(start, static_cast<std::size_t>(length)));
                }
            };
            read_sets(sps_count);
            read_sets(sets.get(1));
            normalized.reserve(data.size());
        }
        Reader nal{data};
        bool idr = false, vcl = false, aud = false, picture_start = false;
        while (nal.pos < nal.b.size()) {
            const auto nal_start = nal.pos;
            const auto length = nal.get(width);
            if (!length || length > nal.b.size() - nal.pos)
                fail("invalid AVC sample NAL length");
            const auto type = nal.b[nal.pos] & 31;
            if (type == 7 || type == 8) {
                if (!strip_matching_config)
                    fail("in-band AVC configuration unsupported");
                const auto candidate = nal.b.subspan(nal.pos, static_cast<std::size_t>(length));
                const bool matches = std::any_of(parameter_sets.begin(), parameter_sets.end(), [&](auto known) {
                    return std::equal(candidate.begin(), candidate.end(), known.begin(), known.end());
                });
                if (!matches)
                    fail("in-band AVC configuration differs from avcC");
                nal.skip(static_cast<std::size_t>(length));
                continue;
            }
            if (type == 13 || type == 15)
                fail("in-band AVC configuration unsupported");
            if (type == 14 || type == 20 || type == 21)
                fail("layered AVC samples unsupported");
            if (nal.b[nal.pos] & 128)
                fail("invalid AVC NAL header");
            if (type == 9) {
                if (aud || vcl)
                    fail("multiple AVC access units in one sample");
                aud = true;
            }
            if (type >= 2 && type <= 4)
                fail("partitioned AVC slices unsupported");
            if (type == 1 || type == 5) {
                const auto first_mb =
                    first_macroblock(nal.b.subspan(nal.pos + 1, static_cast<std::size_t>(length - 1)));
                if (first_mb == 0) {
                    if (picture_start)
                        fail("multiple AVC pictures in one sample");
                    picture_start = true;
                }
                vcl = true;
                if (type == 5)
                    idr = true;
            }
            nal.skip(static_cast<std::size_t>(length));
            if (strip_matching_config) {
                const auto original_nal = data.subspan(nal_start, nal.pos - nal_start);
                normalized.insert(normalized.end(), original_nal.begin(), original_nal.end());
            }
        }
        if (!vcl)
            fail("AVC sample has no coded picture");
        LocSampleCheck check;
        check.independent = idr;
        if (strip_matching_config)
            check.normalized = std::move(normalized);
        return check;
    }
    bool accepts_live_track(std::string_view, std::span<const std::uint8_t> init_data, bool) const override {
        return init_data.size() >= 7 && init_data[0] == 1;
    }
};
} // namespace
const LocCodecProfile &avc_loc_profile() {
    static const AvcLocProfile profile;
    return profile;
}
} // namespace openmoq::publisher
